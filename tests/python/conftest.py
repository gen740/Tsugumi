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
        self._container("run", "-d", "--name", self.name, "-c", "2", "-m", "4G",
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


def demo_environment(tmp_path: Path, demo: str):
    """A fresh copy of a demo served by the container (TSUGUMI_TEST_CONTAINER=1)
    or by a host tsugumid with sandboxing disabled for development."""
    dst = tmp_path / demo
    shutil.copytree(ROOT / "demos" / demo, dst)
    if os.environ.get("TSUGUMI_TEST_CONTAINER") == "1":
        # One store volume per demo keeps Nix downloads across test runs.
        return ContainerEnv(dst, f"tsugumi-test-{demo}").start()
    for d in (dst / "definitions").iterdir():
        card = json.loads((d / "_card.json").read_text())
        if card.get("language"):
            card["sandbox"] = False
            (d / "_card.json").write_text(json.dumps(card))
    daemon = Daemon(dst).start()
    daemon.client_hello = True
    return daemon
