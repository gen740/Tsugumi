"""Core RPC reattaches by the persisted external job ID after a lost reply and crash."""

import hashlib
import json
import socketserver
import struct
import tempfile
import threading
import time
from pathlib import Path

from harness.fixtures import worker_project
from projects import start_run
from Tsugumi.Data.CardState import CardState
from Tsugumi.Data.RemoteExecutionState import RemoteExecutionState
from Tsugumi.Data.RunState import RunState
from tsugumi_client import RpcFailure, path, text


class Gateway(socketserver.UnixStreamServer):
    instance = "1" * 64
    system = "x86_64-linux"

    def __init__(self, endpoint):
        super().__init__(endpoint, Handler)
        self.online = True
        self.receipts = {}
        self.submit_count = 0
        self.blobs = set()

    def operation(self, header, body):
        operation = header["operation"]
        if operation == "catalog":
            return {"services": [{"id": "cluster", "native_system": self.system,
                                  "systems": [self.system], "available": True}]}
        if operation == "probe":
            return {"instance_id": self.instance, "native_system": self.system, "systems": [self.system]}
        assert header["instance_id"] == self.instance
        if operation == "put_blob":
            assert hashlib.sha256(body).hexdigest() == header["sha256"]
            self.blobs.add(header["sha256"])
            return {}
        if not self.online:
            return None  # EOF leaves the job outcome unknown
        job = header["job_id"]
        if operation == "submit":
            self.submit_count += 1
            spec = json.loads(body)
            assert set(spec["input_blobs"]) <= self.blobs
            assert spec["system"] == self.system
            assert spec["cpu_millis"] == 500
            assert spec["memory_bytes"] == 256 << 20
            self.receipts[job] = {"state": "running", "fingerprint": hashlib.sha256(body).hexdigest()}
            return None  # the durable receipt exists, but its response was lost
        if operation == "cancel":
            receipt = self.receipts[job]
            assert receipt["fingerprint"] == header["fingerprint"]
            receipt["state"] = "cancelled"
            return receipt
        assert operation == "status"
        return self.receipts.get(job, {"state": "missing"})


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        size = struct.unpack(">I", self.rfile.read(4))[0]
        header = json.loads(self.rfile.read(size))
        data = self.server.operation(header, self.rfile.read(header.get("size", 0)))
        if data is not None:
            response = json.dumps({"version": 1, "ok": True, **data}).encode()
            self.wfile.write(struct.pack(">I", len(response)) + response)


def wait_for_state(client, run_id, expected):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        try:
            rows = client.call("RunInvocationsRequest", runId=run_id, card=path("work")).records
            if rows and rows[-1].executionState == expected:
                return rows[-1]
        except RpcFailure as error:
            if error.kind != "not_found":
                raise
        time.sleep(0.1)
    raise TimeoutError(f"invocation did not reach remote state {expected}")


def test_core_restart_reattaches_to_the_same_external_job(project, core_factory, tmp_path, monkeypatch):
    """An unknown transport outcome remains pending, and cancellation survives reconnection."""
    with tempfile.TemporaryDirectory(prefix="tsg-remote-", dir="/tmp") as directory:
        endpoint = str(Path(directory) / "broker")
        monkeypatch.setenv("TSUGUMI_EXECUTION_BROKER", "unix:" + endpoint)
        monkeypatch.delenv("TSUGUMI_EXECUTION_BROKER_TOKEN_FILE", raising=False)
        with Gateway(endpoint) as gateway:
            server = threading.Thread(target=gateway.serve_forever, kwargs={"poll_interval": 0.02})
            server.start()
            daemon = None
            try:
                daemon = core_factory(mocked_builds=True)
                client = daemon.connect()
                worker_project(client, project, tmp_path, [], sandbox=True, executionService="cluster",
                               executionResources={"cpuMillis": 500, "memoryBytes": 256 << 20})
                run_id = start_run(client, "work", propagate=False)
                live = wait_for_state(client, run_id, RemoteExecutionState.running)
                job_id = text(live.executionJobId)
                assert gateway.submit_count == 1

                gateway.online = False
                wait_for_state(client, run_id, RemoteExecutionState.disconnected)
                daemon.restart(crash=True)
                client = daemon.connect()
                assert client.call("RunStatusRequest", runId=run_id).state == RunState.failed
                pending = wait_for_state(client, run_id, RemoteExecutionState.disconnected)
                assert text(pending.executionJobId) == job_id

                client.call("RunCancelRequest", runId=run_id)
                gateway.online = True
                finished = wait_for_state(client, run_id, RemoteExecutionState.cancelled)
                assert finished.state == CardState.cancelled
                assert text(finished.executionJobId) == job_id
                assert gateway.submit_count == 1
            finally:
                if daemon:
                    daemon.stop()
                gateway.shutdown()
                server.join()
