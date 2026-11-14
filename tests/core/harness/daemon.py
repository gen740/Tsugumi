"""Own a real daemon process; expose only its external RPC to scenarios."""

from __future__ import annotations

import json
import os
import shutil
import signal
import subprocess
import tempfile
import time
from pathlib import Path

from tsugumi_client import Client

ROOT = Path(__file__).resolve().parents[3]
BUILD = Path(os.environ.get("TSUGUMI_BUILD_DIR", ROOT / "build")).resolve()
DAEMON = Path(os.environ.get("TSUGUMID", BUILD / "core/tsugumid"))
MOCK_DAEMON = Path(os.environ.get("TSUGUMI_MOCK_DAEMON", BUILD / "tests/core/tsugumid-mock"))
WORKER = Path(os.environ.get("TSUGUMI_WORKER", BUILD / "workerd/tsugumi-worker"))
SDK = Path(os.environ.get("TSUGUMI_SDK_DIR", ROOT / "sdk"))

# Adapter to existing crash points, not a contract asserted by test cases.
FAULTS = {
    "save_before_commit": "before_commit",
    "save_after_prepare": "after_journal",
    "save_during_apply": "partial_apply",
    "invocation_record": "history_invocation",
    "run_acceptance": "run_acceptance",
    "archive_after_main_move": "history_archive_after_main",
    "save_exception": "save_throws",
}


def write_project(root: Path) -> None:
    root.mkdir(parents=True, exist_ok=True)
    (root / "tsugumi.json").write_text(json.dumps({
        "name": "rpc-test", "pkgsPath": "tsugumi:default",
        "canvas": {"cards": [], "connections": []},
    }))
    (root / "layout.json").write_text("{}")


class Daemon:
    def __init__(self, project: Path, *, mocked_builds=False, worker=None, fault=None,
                 history_dir=None, args=None):
        self.project = project
        self.binary = MOCK_DAEMON if mocked_builds else DAEMON
        self.worker = Path(worker or WORKER)
        self.fault = fault
        self.args = list(args or [])
        if history_dir is not None:
            self.args += ["--history-dir", str(history_dir)]
        self.directory = Path(tempfile.mkdtemp(prefix="tsg-rpc-"))
        self.unix = str(self.directory / "rpc")
        self.control = self.directory / "control"
        self.gates = self.directory / "gates"
        self.gates.mkdir()
        self.log = self.directory / "daemon.log"
        self.ready = {}
        self.proc = None
        self.clients = []

    def start(self, timeout=20):
        if self.proc and self.proc.poll() is None:
            raise RuntimeError("daemon already running")
        if not self.binary.is_file():
            raise FileNotFoundError(f"build the test targets first: {self.binary}")
        ready = self.control / "server.ready"
        ready.unlink(missing_ok=True)
        env = dict(os.environ)
        env.pop("TSUGUMI_FAULT", None)
        env.pop("TSUGUMI_ENABLE_FAULTS", None)
        env["TSUGUMI_MOCK_CONTROL"] = str(self.gates)
        if self.fault:
            env.update(TSUGUMI_ENABLE_FAULTS="1", TSUGUMI_FAULT=FAULTS[self.fault])
        args = [str(self.binary), "--project", str(self.project), "--unix", self.unix,
                "--listen", "127.0.0.1:0", "--control-dir", str(self.control),
                "--worker", str(self.worker), "--sdk-dir", str(SDK),
                "--allow-origin", "http://rpc.test", "--cpus", "2"]
        args += self.args
        try:
            with self.log.open("ab") as log:
                self.proc = subprocess.Popen(args, env=env, stdout=log, stderr=log)
            end = time.monotonic() + timeout
            while time.monotonic() < end:
                if self.proc.poll() is not None:
                    raise RuntimeError(f"daemon exited ({self.proc.returncode}):\n{self.diagnostics()}")
                if ready.exists():
                    self.ready = json.loads(ready.read_text())
                    return self
                time.sleep(0.01)
            raise TimeoutError(f"daemon startup timed out:\n{self.diagnostics()}")
        except BaseException:
            self.stop()
            raise

    def connect(self, *, websocket=False, unix=None, language=None):
        if websocket:
            c = Client(ws=self.ready["endpoint"], origin="http://rpc.test")
        else:
            c = Client(unix=unix or self.unix)
        self.clients.append(c)
        c.hello(token=self.ready["token"] if websocket else None, language=language)
        return c

    def diagnostics(self):
        return self.log.read_text(errors="replace")[-12000:] if self.log.exists() else "no daemon log"

    def stop(self, *, crash=False):
        for c in self.clients:
            c.close()
        self.clients.clear()
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGKILL if crash else signal.SIGTERM)
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)

    def restart(self, *, fault=None, crash=False):
        self.stop(crash=crash)
        self.fault = fault
        return self.start()

    def close(self):
        self.stop()
        shutil.rmtree(self.directory, ignore_errors=True)

    def __enter__(self):
        try:
            return self.start()
        except BaseException:
            self.close()
            raise

    def __exit__(self, *_):
        self.close()
