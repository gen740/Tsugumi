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
