"""Fixtures: a fresh project and a running tsugumid serving it."""

from __future__ import annotations

import json
import os
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
# Also support the documentation player harvester launched directly.
sys.path.insert(0, str(ROOT / "tests" / "core"))
BUILD = Path(os.environ.get("TSUGUMI_BUILD_DIR", ROOT / "build"))
TSUGUMID = Path(os.environ.get("TSUGUMID", BUILD / "tsugumid" / "tsugumid"))
WORKER = Path(os.environ.get("TSUGUMI_WORKER", BUILD / "workerd" / "tsugumi-worker"))
SDK = Path(os.environ.get("TSUGUMI_SDK_DIR", ROOT / "sdk"))
WEB_DIST = Path(os.environ.get("TSUGUMI_WEB_DIST", ROOT / "ui/web" / "dist"))
# The tsugumi command (cli/), and the built page hostd serves.
CLI = Path(os.environ.get("TSUGUMI_CLI", BUILD / "cli" / "tsugumi"))
HOSTD = Path(os.environ.get("TSUGUMI_HOSTD", BUILD / "hostd" / "tsugumi-hostd"))
WEB_ROOT = WEB_DIST / "public"

EMPTY_PROJECT = {
    "name": "test",
    "pkgsPath": "tsugumi:default",
    "canvas": {"cards": [], "connections": []},
}


def write_project(root: Path, project: dict | None = None, layout: dict | None = None) -> None:
    root.mkdir(parents=True, exist_ok=True)
    (root / "tsugumi.json").write_text(json.dumps(project or EMPTY_PROJECT))
    (root / "layout.json").write_text(json.dumps(layout or {}))


from harness.daemon import Daemon as CoreDaemon


class Daemon(CoreDaemon):
    def __init__(self, project: Path, env: dict | None = None, args: list | None = None):
        super().__init__(project, env=env, args=["--allow-missing-origin", *(args or [])])
        self.binary = TSUGUMID

    def client(self):
        from tsugumi_client import Client
        client = Client(unix=self.unix)
        self.clients.append(client)
        return client

    def crash(self):
        self.stop(crash=True)

    def cleanup(self):
        self.close()


@pytest.fixture
def project(tmp_path: Path) -> Path:
    root = tmp_path / "project"
    write_project(root)
    return root


@pytest.fixture
def daemon(project: Path):
    d = Daemon(project)
    try:
        d.start()
        yield d
    finally:
        d.cleanup()


class SharedVM:
    """The app's shared VM driven through tsugumi-vmctl: one per test session,
    with private disposable storage. vm/macos/tools/build-vmctl.sh builds and signs it."""

    _instance: "SharedVM | None" = None

    def __init__(self, *, rpc_trace=False):
        import atexit
        vmctl = os.environ.get("TSUGUMI_VMCTL", str(ROOT / "cli/vmctl" / ".build" / "debug" / "tsugumi-vmctl"))
        self.directory = tempfile.TemporaryDirectory(prefix="vm-")
        support = Path(self.directory.name)
        support.mkdir(parents=True, exist_ok=True)
        self.console = support / "vmctl-console.log"
        env = dict(os.environ, TSUGUMI_SOCKET_DIR=str(support),
                   TSUGUMI_RPC_TRACE="1" if rpc_trace else "0",
                   TSUGUMI_RUN_SCRATCH_DIR=str(support.resolve() / "scratch"))
        with self.console.open("a") as log:
            self.proc = subprocess.Popen([vmctl, "--support", str(support), "--console"], stdin=subprocess.PIPE,
                                         stdout=subprocess.PIPE, stderr=log, text=True, bufsize=1, env=env)
        # Queue replies and EOF so callers wait for the console protocol.
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

    def command(self, line: str, until=lambda l: True, timeout: float | None = None) -> list[str]:
        """Sends one command and reads its answer up to the line `until` accepts.
        Quit has a grace period so fixture cleanup can proceed if VM shutdown stalls."""
        self.proc.stdin.write(line + '''
''')
        self.proc.stdin.flush()
        out = []
        deadline = None if timeout is None else time.monotonic() + timeout
        while True:
            try:
                remaining = None if deadline is None else max(0.0, deadline - time.monotonic())
                l = self.lines.get(timeout=remaining)
            except queue.Empty:
                self.proc.terminate()
                try:
                    self.proc.wait(timeout=47)
                except subprocess.TimeoutExpired:
                    self.proc.kill()
                raise TimeoutError(f"tsugumi-vmctl did not answer {line.split()[0]!r} within {timeout:.0f} s; see {self.console}")
            if l is None:
                raise RuntimeError(f"tsugumi-vmctl exited; see {self.console}")
            out.append(l)
            if until(l):
                return out

    def quit(self):
        if self.proc.stdin.closed:
            return
        if self.proc.poll() is None:
            try:
                self.command("quit", until=lambda l: l == "bye", timeout=120)
                self.proc.wait(timeout=60)
            except Exception:
                self.proc.terminate()
                try:
                    self.proc.wait(timeout=47)
                except subprocess.TimeoutExpired:
                    self.proc.kill()
                self.proc.wait()
        self.proc.stdin.close()
        self.proc.stdout.close()
        # The controller asks for orderly shutdown; clean up any remaining
        # owned resident services with the same bounded signal path.
        import importlib.util
        spec = importlib.util.spec_from_file_location("test_runner_cleanup", ROOT / "tests/run-isolated.py")
        cleanup = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cleanup)
        cleanup.stop_services(Path(self.directory.name))
        self.directory.cleanup()


class VMEnv:
    """Runs a project in the app's shared VM (the reference configuration)."""

    def __init__(self, project: Path, *, vm: SharedVM | None = None):
        self.project = project
        self.vm = vm if vm is not None else SharedVM.get()
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
        """Abnormal termination: await the old core's actual transport EOF."""
        with self.client() as observer:
            self.exec("for f in /proc/[0-9]*/comm; do [ \"$(< $f)\" = tsugumid ] && p=${f#/proc/} && kill -9 ${p%/comm}; done; true")
            observer.wait_closed()

    def stop(self):
        if self.id:
            self.vm.command(f"stop {self.project}")

    def cleanup(self):
        self.stop()


def real_environment(project: Path):
    """The real execution environment: the shared VM."""
    return VMEnv(project).start()


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
        return real_environment(dst)
    # Every Card's own definition, also inside its Cards in Card (externals).
    for f in dst.rglob("definitions/*/_card.json"):
        card = json.loads(f.read_text())
        if card.get("language"):
            card["sandbox"] = {"filesystem": False}
            f.write_text(json.dumps(card))
    daemon = Daemon(dst).start()
    daemon.client_hello = True
    return daemon
