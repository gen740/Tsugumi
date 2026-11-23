"""Own a real daemon process; expose only its external RPC to scenarios."""

from __future__ import annotations

import json
import os
import shutil
import signal
import subprocess
import tempfile
from pathlib import Path

from tsugumi_client import Client

from .control import read_ready, read_startup_error
from .file_events import process_file
from .nix_path import declarations, test_lock_entries, write_lock

ROOT = Path(__file__).resolve().parents[3]
BUILD = Path(os.environ.get("TSUGUMI_BUILD_DIR", ROOT / "build")).resolve()
DAEMON = Path(os.environ.get("TSUGUMID", BUILD / "tsugumid/tsugumid"))
MOCK_DAEMON = Path(os.environ.get("TSUGUMI_MOCK_DAEMON", BUILD / "tests/core/tsugumid-mock"))
WORKER = Path(os.environ.get("TSUGUMI_WORKER", BUILD / "workerd/tsugumi-worker"))
WORKERD = Path(os.environ.get("TSUGUMI_WORKERD", WORKER.parent / "tsugumi-workerd"))
NIX = Path(os.environ.get("TSUGUMI_NIX_DIR", BUILD / "share/tsugumi/nix"))

# Adapter to existing crash points, not a contract asserted by test cases.
FAULTS = {
    "save_before_commit": "before_commit",
    "save_after_prepare": "after_prepare",
    "save_during_apply": "partial_apply",
    "invocation_record": "history_invocation",
    "run_acceptance": "run_acceptance",
    "save_exception": "save_throws",
    "transfer_gate": "transfer_gate",
}


def write_project(root: Path) -> None:
    root.mkdir(parents=True, exist_ok=True)
    entries = test_lock_entries()
    (root / "tsugumi.json").write_text(json.dumps({
        "name": "rpc-test", "nixPath": declarations(entries),
        "canvas": {"cards": [], "connections": []},
    }))
    write_lock(root, entries)


class StartupFailure(RuntimeError):
    def __init__(self, returncode, record, diagnostics):
        self.returncode = returncode
        self.record = record
        self.kind = record.get("kind")
        super().__init__(f"daemon exited ({returncode}):\n{diagnostics}")


class Daemon:
    def __init__(self, project: Path, *, mocked_builds=False, worker=None, fault=None,
                 args=None, env=None):
        self.project = project
        self.env = env or {}
        self.binary = MOCK_DAEMON if mocked_builds else DAEMON
        self.worker = Path(worker or WORKER)
        self.fault = fault
        self.args = list(args or [])
        self.directory = Path(tempfile.mkdtemp(prefix="tsugumi-rpc-", dir=os.environ.get("TSUGUMI_SOCKET_DIR", "/tmp"))).resolve()
        self.unix = str(self.directory / "rpc")
        self.control = self.directory / "control"
        self.gates = self.directory / "gates"
        self.gates.mkdir()
        self.log = self.directory / "daemon.log"
        self.ready = {}
        self.proc = None
        self.clients = []

    def start(self, expect_exit=False):
        if self.proc and self.proc.poll() is None:
            raise RuntimeError("daemon already running")
        ready = self.control / "server.ready"
        ready.unlink(missing_ok=True)
        env = dict(os.environ)
        env.pop("TSUGUMI_FAULT", None)
        env.pop("TSUGUMI_ENABLE_FAULTS", None)
        env["TSUGUMI_MOCK_CONTROL"] = str(self.gates)
        if self.fault:
            env.update(TSUGUMI_ENABLE_FAULTS="1", TSUGUMI_FAULT=FAULTS[self.fault])
        env.update(self.env)
        args = [str(self.binary), "--scratch-dir", str(self.directory / "scratch"),
                "--run-scratch-dir", str(self.directory / "run-scratch"),
                "--project", str(self.project), "--unix", self.unix,
                "--listen", "127.0.0.1:0", "--control-dir", str(self.control),
                "--worker", str(self.worker), "--workerd", str(WORKERD), "--nix-dir", str(NIX),
                "--allow-origin", "http://rpc.test", "--cpus", "2"]
        args += self.args
        try:
            if not self.binary.is_file():
                raise FileNotFoundError(f"build the test targets first: {self.binary}")
            with self.log.open("ab") as log:
                self.proc = subprocess.Popen(args, env=env, stdout=log, stderr=log)
            if process_file(self.proc, ready) is not None:
                self.ready = read_ready(ready)
                return self
            status = self.proc.wait()
            if expect_exit:
                return self
            record_path = self.control / "server.startup-error"
            record = read_startup_error(record_path) if record_path.exists() else {}
            raise StartupFailure(status, record, self.diagnostics())
        except BaseException:
            self.close()
            raise

    def connect(self, *, websocket=False, unix=None):
        if websocket:
            c = Client(ws=self.ready["endpoint"], origin="http://rpc.test")
        else:
            c = Client(unix=unix or self.unix)
        self.clients.append(c)
        c.hello(token=self.ready["token"] if websocket else None)
        return c

    def diagnostics(self):
        return self.log.read_text(errors="replace")[-12000:] if self.log.exists() else "no daemon log"

    def stop(self, *, crash=False):
        for c in self.clients:
            c.close()
        self.clients.clear()
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGKILL if crash else signal.SIGTERM)
            # An unresponsive service must not prevent cleanup of later fixtures.
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()

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
