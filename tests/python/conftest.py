"""Fixtures: a fresh project and a running tsugumid serving it."""

from __future__ import annotations

import json
import os
import shutil
import signal
import subprocess
import tempfile
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
BUILD = Path(os.environ.get("TSUGUMI_BUILD_DIR", ROOT / "build"))
TSUGUMID = Path(os.environ.get("TSUGUMID", BUILD / "core" / "tsugumid"))
WORKER = Path(os.environ.get("TSUGUMI_WORKER", BUILD / "sdk" / "tsugumi-worker"))

EMPTY_PROJECT = {
    "name": "test",
    "pkgsPath": "tsugumi:default",
    "databaseConnections": [],
    "canvas": {"cards": [], "connections": []},
}


def write_project(root: Path, project: dict | None = None, layout: dict | None = None) -> None:
    root.mkdir(parents=True, exist_ok=True)
    (root / "tsugumi.json").write_text(json.dumps(project or EMPTY_PROJECT))
    (root / "layout.json").write_text(json.dumps(layout or {}))


class Daemon:
    def __init__(self, project: Path, env: dict | None = None, listen: bool = False):
        self.project = project
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
                "--sdk-dir", str(ROOT / "sdk"), "--allow-missing-origin"]
        if self.listen:
            args += ["--listen", "127.0.0.1:0"]
        env = dict(os.environ)
        env.update(self.env)
        ready = self.control / "server.ready"
        if ready.exists():
            ready.unlink()
        self.proc = subprocess.Popen(args, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                if expect_exit:
                    return self
                raise RuntimeError("tsugumid exited: " + self.proc.stderr.read().decode())
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


class ContainerEnv:
    """Runs a project inside the real Apple container environment image."""

    image = "tsugumi-env:1.0.0"

    def __init__(self, project: Path, name: str):
        self.project = project
        self.name = name
        self.control = Path(tempfile.mkdtemp(prefix="tsgc-"))
        os.chmod(self.control, 0o700)
        self.volume = f"{name}-store"
        self.port = None
        self.ready: dict = {}

    def _container(self, *args, check=True, timeout=600):
        return subprocess.run(["container", *args], check=check, capture_output=True, text=True, timeout=timeout)

    def start(self) -> "ContainerEnv":
        import socket
        self._container("rm", "-f", self.name, check=False)
        if self._container("volume", "inspect", self.volume, check=False).returncode != 0:
            self._container("volume", "create", "-s", "20G", self.volume)
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            self.port = s.getsockname()[1]
        ready = self.control / "server.ready"
        if ready.exists():
            ready.unlink()
        # The environment's guest kernel (macOS/Container/kernel.nix) when built.
        kernel = os.environ.get("TSUGUMI_KERNEL", str(ROOT / "dist" / "tsugumi-kernel"))
        kernel_args = ["--kernel", kernel] if os.path.exists(kernel) else []
        self._container("run", "-d", "--name", self.name, *kernel_args, "-c", "2", "-m", "4G",
                        "-v", f"{self.project}:/project", "-v", f"{self.control}:/control",
                        "-v", f"{self.volume}:/persist", "-p", f"127.0.0.1:{self.port}:7700",
                        "-e", "TSUGUMI_CPUS=2", self.image)
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            if ready.exists():
                self.ready = json.loads(ready.read_text())
                return self
            time.sleep(0.1)
        raise TimeoutError(self._container("logs", self.name, check=False).stdout)

    def client(self):
        from tsugumi_client import Client
        c = Client(ws=f"ws://127.0.0.1:{self.port}/rpc")
        c.hello(token=self.ready["token"])
        return c

    def crash(self):
        """Abnormal termination: the VM's main process is killed."""
        self._container("kill", "--signal", "KILL", self.name, check=False)
        time.sleep(1)

    def exec(self, script: str) -> str:
        return subprocess.run(["container", "exec", self.name, "bash", "-c", script], capture_output=True, text=True).stdout

    def stop(self):
        # TSUGUMI_TEST_LOGS=<dir> keeps the environment's log for diagnosis.
        if os.environ.get("TSUGUMI_TEST_LOGS"):
            out = Path(os.environ["TSUGUMI_TEST_LOGS"]) / f"{self.name}-{int(time.time())}.log"
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_text(self._container("logs", self.name, check=False).stdout)
        try:
            self._container("stop", "--time", "20", self.name, check=False, timeout=120)
        except subprocess.TimeoutExpired:
            # A VM that does not shut down is killed; the test result stands.
            self._container("kill", "--signal", "KILL", self.name, check=False, timeout=120)
        self._container("rm", "-f", self.name, check=False, timeout=120)

    def cleanup(self, keep_volume: bool = True):
        self.stop()
        if not keep_volume:
            self._container("volume", "rm", self.volume, check=False)
        shutil.rmtree(self.control, ignore_errors=True)


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
        atexit.register(self.quit)

    @classmethod
    def get(cls) -> "SharedVM":
        if cls._instance is None or cls._instance.proc.poll() is not None:
            cls._instance = SharedVM()
        return cls._instance

    def command(self, line: str, until=lambda l: True) -> list[str]:
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        out = []
        while True:
            l = self.proc.stdout.readline()
            if not l:
                raise RuntimeError(f"tsugumi-vmctl exited; see {self.console}")
            out.append(l.rstrip("\n"))
            if until(out[-1]):
                return out

    def quit(self):
        if self.proc.poll() is None:
            try:
                self.command("quit", until=lambda l: l == "bye")
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
        # Terminal control sequences (bracketed paste and the like) are not output.
        return re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", "\n".join(out[:-1]))

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
    """The real execution environment: the shared VM, or with
    TSUGUMI_TEST_ENV=container the Apple container of earlier versions."""
    if os.environ.get("TSUGUMI_TEST_ENV") == "container":
        return ContainerEnv(project, name).start()
    return VMEnv(project, name).start()


def demo_environment(tmp_path: Path, demo: str):
    """A fresh copy of a demo served by the container (TSUGUMI_TEST_CONTAINER=1)
    or by a host tsugumid with sandboxing disabled for development."""
    dst = tmp_path / demo
    shutil.copytree(ROOT / "demos" / demo, dst)
    if os.environ.get("TSUGUMI_TEST_CONTAINER") == "1":
        # One store per demo (container) or the one shared store (VM) keeps Nix downloads.
        return real_environment(dst, f"tsugumi-test-{demo}")
    for d in (dst / "definitions").iterdir():
        card = json.loads((d / "_card.json").read_text())
        if card.get("language"):
            card["sandbox"] = False
            (d / "_card.json").write_text(json.dumps(card))
    daemon = Daemon(dst).start()
    daemon.client_hello = True
    return daemon
