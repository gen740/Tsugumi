"""Fixtures: a fresh project and a running tsugumid serving it."""

from __future__ import annotations

import json
import os
import queue
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
# Also support the measurement scripts launched directly from this folder.
sys.path.insert(0, str(ROOT / "tests" / "core"))
BUILD = Path(os.environ.get("TSUGUMI_BUILD_DIR", ROOT / "build"))
TSUGUMID = Path(os.environ.get("TSUGUMID", BUILD / "core" / "tsugumid"))
WORKER = Path(os.environ.get("TSUGUMI_WORKER", BUILD / "workerd" / "tsugumi-worker"))
SDK = Path(os.environ.get("TSUGUMI_SDK_DIR", ROOT / "sdk"))
WEB_DIST = Path(os.environ.get("TSUGUMI_WEB_DIST", ROOT / "web" / "dist"))

EMPTY_PROJECT = {
    "name": "test",
    "pkgsPath": "tsugumi:default",
    "canvas": {"cards": [], "connections": []},
}


def write_project(root: Path, project: dict | None = None, layout: dict | None = None) -> None:
    root.mkdir(parents=True, exist_ok=True)
    (root / "tsugumi.json").write_text(json.dumps(project or EMPTY_PROJECT))
    (root / "layout.json").write_text(json.dumps(layout or {}))


class Daemon:
    def __init__(self, project: Path, env: dict | None = None, listen: bool = False, args: list | None = None):
        self.project = project
        self.args = list(args or [])
        # Unix socket paths are short-limited; keep them under /tmp-like dirs.
        self.sock_dir = Path(tempfile.mkdtemp(prefix="tsg-"))
        self.unix = str(self.sock_dir / "s")
        self.control = self.sock_dir / "control"
        self.listen = listen
        self.env = env or {}
        self.proc: subprocess.Popen | None = None
        self.ready: dict = {}

    def start(self, expect_exit: bool = False, timeout: float = 20.0) -> "Daemon":
        args = [str(TSUGUMID), "--project", str(self.project), "--unix", self.unix,
                "--control-dir", str(self.control), "--worker", str(WORKER),
                "--sdk-dir", str(SDK), "--allow-missing-origin"]
        if self.listen:
            args += ["--listen", "127.0.0.1:0"]
        args += self.args
        env = dict(os.environ)
        env.update(self.env)
        ready = self.control / "server.ready"
        if ready.exists():
            ready.unlink()
        # Its output goes to a file (a pipe nobody reads would stop it once full).
        self.log = self.sock_dir / "tsugumid.log"
        with open(self.log, "ab") as log:
            self.proc = subprocess.Popen(args, env=env, stdout=log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                if expect_exit:
                    return self
                raise RuntimeError("tsugumid exited: " + self.log.read_text(errors="replace")[-4000:])
            if ready.exists():
                self.ready = json.loads(ready.read_text())
                return self
            time.sleep(0.02)
        raise TimeoutError("tsugumid did not become ready")

    def stop(self, sig: int = signal.SIGTERM) -> int:
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(sig)
            try:
                self.proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        return self.proc.returncode if self.proc else 0

    def client(self):
        from tsugumi_client import Client
        return Client(unix=self.unix)

    def crash(self):
        """Abnormal termination of the owner process."""
        self.proc.kill()
        self.proc.wait()

    def cleanup(self) -> None:
        self.stop()
        shutil.rmtree(self.sock_dir, ignore_errors=True)


@pytest.fixture
def project(tmp_path: Path) -> Path:
    root = tmp_path / "project"
    write_project(root)
    return root


@pytest.fixture
def daemon(project: Path):
    d = Daemon(project).start()
    yield d
    d.cleanup()


@pytest.fixture
def client(daemon):
    c = daemon.client()
    c.hello()
    yield c
    c.close()


class SharedVM:
    """The app's shared VM driven through tsugumi-vmctl: one per test session,
    with its Nix store kept in TSUGUMI_TEST_VM_SUPPORT (build/test-vm) so
    downloads survive test runs. macOS/tools/build-vmctl.sh builds and signs it."""

    _instance: "SharedVM | None" = None

    def __init__(self):
        import atexit
        vmctl = os.environ.get("TSUGUMI_VMCTL", str(ROOT / "macOS" / ".build" / "debug" / "tsugumi-vmctl"))
        support = Path(os.environ.get("TSUGUMI_TEST_VM_SUPPORT", BUILD / "test-vm"))
        support.mkdir(parents=True, exist_ok=True)
        self.console = support / "vmctl-console.log"
        self.proc = subprocess.Popen([vmctl, "--support", str(support), "--console"], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=open(self.console, "a"), text=True, bufsize=1)
        # Its answers are read by a thread so that every wait has a deadline.
        self.lines: queue.Queue = queue.Queue()
        threading.Thread(target=self._pump, daemon=True).start()
        atexit.register(self.quit)

    def _pump(self):
        for line in self.proc.stdout:
            self.lines.put(line.rstrip('''
'''))
        self.lines.put(None)

    @classmethod
    def get(cls) -> "SharedVM":
        if cls._instance is None or cls._instance.proc.poll() is not None:
            cls._instance = SharedVM()
        return cls._instance

    def command(self, line: str, until=lambda l: True, timeout: float = 900.0) -> list[str]:
        """Sends one command and reads its answer up to the line `until` accepts.
        A vmctl that does not answer in time is ended (the next test starts a
        new one) and the test fails with where to look."""
        self.proc.stdin.write(line + '''
''')
        self.proc.stdin.flush()
        out = []
        deadline = time.monotonic() + timeout
        while True:
            try:
                l = self.lines.get(timeout=max(0.0, deadline - time.monotonic()))
            except queue.Empty:
                self.proc.kill()
                raise TimeoutError(f"tsugumi-vmctl did not answer {line.split()[0]!r} within {timeout:.0f} s; see {self.console}")
            if l is None:
                raise RuntimeError(f"tsugumi-vmctl exited; see {self.console}")
            out.append(l)
            if until(l):
                return out

    def quit(self):
        if self.proc.poll() is None:
            try:
                self.command("quit", until=lambda l: l == "bye", timeout=120)
                self.proc.wait(timeout=60)
            except Exception:
                self.proc.kill()


class VMEnv:
    """Runs a project in the app's shared VM (the reference configuration)."""

    def __init__(self, project: Path, name: str = ""):
        self.project = project
        self.name = name
        self.vm = SharedVM.get()
        self.port = None
        self.ready: dict = {}
        self.id = ""
        self.control = None

    def start(self) -> "VMEnv":
        line = self.vm.command(f"start {self.project}")[-1]
        if not line.startswith("ready "):
            raise RuntimeError(line)
        _, self.id, url, control = line.split(" ", 3)
        self.port = int(url.split(":")[2].split("/")[0])
        self.control = Path(control)
        self.ready = json.loads((self.control / "server.ready").read_text())
        return self

    def client(self):
        from tsugumi_client import Client
        c = Client(ws=f"ws://127.0.0.1:{self.port}/rpc")
        c.hello(token=self.ready["token"])
        return c

    def exec(self, script: str) -> str:
        """Runs a one-line bash script in the project's terminal environment."""
        import re
        out = self.vm.command(f"sh {self.id} {script}", until=lambda l: l.startswith("exit ") or l.startswith("error"))
        # Terminal control sequences (bracketed paste and the like) are not
        # output, nor is the echo of the final `exit`.
        lines = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", '''
'''.join(out[:-1])).split('''
''')
        while lines and lines[-1].strip() in ("", "exit"):
            lines.pop()
        return '''
'''.join(lines)

    def crash(self):
        """Abnormal termination: the project's tsugumid is killed."""
        self.exec("for f in /proc/[0-9]*/comm; do [ \"$(< $f)\" = tsugumid ] && p=${f#/proc/} && kill -9 ${p%/comm}; done; true")
        time.sleep(1)

    def stop(self):
        if self.id:
            self.vm.command(f"stop {self.project}")

    def cleanup(self, keep_volume: bool = True):
        self.stop()


def real_environment(project: Path, name: str):
    """The real execution environment: the shared VM."""
    return VMEnv(project, name).start()


def demo_environment(tmp_path: Path, demo: str, edit=None, language: str = "ja"):
    """A fresh copy of a demo (of demos/<language>) served by the shared VM
    (TSUGUMI_TEST_CONTAINER=1) or by a host tsugumid with sandboxing disabled
    for development. `edit(copy)` changes the copy before it is served."""
    dst = tmp_path / demo
    shutil.copytree(ROOT / "demos" / language / demo, dst)
    if edit:
        edit(dst)
    if os.environ.get("TSUGUMI_TEST_CONTAINER") == "1":
        # The one shared store of the VM keeps Nix downloads.
        return real_environment(dst, f"tsugumi-test-{demo}")
    # Every Card's own definition, also inside its Cards in Card (externals).
    for f in dst.rglob("definitions/*/_card.json"):
        card = json.loads(f.read_text())
        if card.get("language"):
            card["sandbox"] = False
            f.write_text(json.dumps(card))
    daemon = Daemon(dst).start()
    daemon.client_hello = True
    return daemon
