"""Core RPC reattaches by the persisted external job ID after a lost reply and crash."""

import hashlib
import json
import signal
import socketserver
import struct
import tempfile
import threading
import time
from pathlib import Path

import flatbuffers
import pytest
from harness.fixtures import worker_project
from projects import FILE_FBS, start_run, wait_run
from Tsugumi.Data.CardState import CardState
from Tsugumi.Data.LogStream import LogStream
from Tsugumi.Data.RemoteExecutionState import RemoteExecutionState
from Tsugumi.Data.RunState import RunState
from Tsugumi.Worker.WorkerMessage import WorkerMessageT
from Tsugumi.Worker.WorkerOutput import WorkerOutputT
from Tsugumi.Worker.WorkerPayload import WorkerPayload
from Tsugumi.Worker.WorkerResult import WorkerResultT
from tsugumi_client import RpcFailure, path, text


class Gateway(socketserver.UnixStreamServer):
    instance = "1" * 64
    server_id = "2" * 64
    system = "x86_64-linux"

    def __init__(self, endpoint):
        super().__init__(endpoint, Handler)
        self.online = True
        self.registered_server = self.server_id
        self.resource_limits = True
        self.probe_overrides = {}
        self.receipts = {}
        self.submit_count = 0
        self.blobs = set()
        self.results = {}
        self.stall = None
        self.stall_operation = "status"
        self.stalled = threading.Event()
        self.resume = threading.Event()

    def finish(self, job, *, result_fault=None):
        def blob(data):
            digest = hashlib.sha256(data).hexdigest()
            self.results[digest] = data
            return digest

        value = flatbuffers.Builder(64)
        output_path = value.CreateString("out/result.txt")
        value.StartObject(1)
        value.PrependUOffsetTRelativeSlot(0, output_path, 0)
        value.Finish(value.EndObject())
        value_bytes = b"\x00" if result_fault == "type" else value.Output()
        result = WorkerResultT(outputs=[WorkerOutputT(name="doc", bytes=value_bytes)])
        message = WorkerMessageT(callId=1, valueType=WorkerPayload.WorkerResult, value=result)
        buffer = flatbuffers.Builder(128)
        buffer.Finish(message.Pack(buffer), file_identifier=b"TSW1")
        files = {
            "files/out/result.txt": b"retained after shutdown\n",
            "environment.json": b'{"paths":["/nix/store/00000000000000000000000000000000-mock"],"directories":["out"],"built":true}',
            "logs.jsonl": b'{"stream":1,"text":"one recovered message\\n"}\n',
        }
        entries = [{"size": len(data), "sha256": blob(data), "path": path} for path, data in files.items()]
        if result_fault == "escape":
            entries[0]["path"] = "files/out/../../escape"
        if result_fault == "oversize":
            next(entry for entry in entries if entry["path"] == "logs.jsonl")["size"] = (16 << 20) + 1
        manifest = json.dumps({
            "outputs": entries,
            "invocation_result_sha256": blob(bytes(buffer.Output())),
        }).encode()
        self.receipts[job] = {**self.receipts[job], "state": "succeeded", "result_sha256": blob(manifest)}

    def operation(self, header, body):
        operation = header["operation"]
        capabilities = {
            "server_id": self.registered_server, "native_system": self.system, "systems": [self.system],
            "executor": "daemon", "isolation": "pid_namespace", "resource_limits": self.resource_limits,
            "max_jobs": 8, "worker_contract": 2,
        }
        if operation == "catalog":
            return {"services": [{"id": "cluster", "available": True, **capabilities}]}
        assert header["server_id"] == self.server_id
        if header["server_id"] != self.registered_server:
            return {"ok": False, "code": "registration_changed", "error": "execution service was replaced"}
        if operation == "probe":
            return {key: value for key, value in {
                "instance_id": self.instance, **capabilities, **self.probe_overrides,
            }.items() if value is not None}
        assert header["instance_id"] == self.instance
        if operation == "put_blob":
            assert hashlib.sha256(body).hexdigest() == header["sha256"]
            self.blobs.add(header["sha256"])
            return {}
        if not self.online:
            return None  # EOF leaves the job outcome unknown
        if operation == "get_blob":
            return self.results[header["sha256"]]
        job = header["job_id"]
        if operation == "submit":
            self.submit_count += 1
            spec = json.loads(body)
            assert set(spec["input_blobs"]) <= self.blobs
            assert spec["system"] == self.system
            if self.resource_limits:
                assert spec["cpu_millis"] == 500
                assert spec["memory_bytes"] == 256 << 20
            else:
                assert "cpu_millis" not in spec
                assert "memory_bytes" not in spec
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
        prefix = self.rfile.read(4)
        if len(prefix) != 4:
            return
        size = struct.unpack(">I", prefix)[0]
        header = json.loads(self.rfile.read(size))
        mode = self.server.stall
        if mode and header["operation"] == self.server.stall_operation:
            try:
                if mode == "trickle":
                    self.wfile.write(struct.pack(">I", 8192) + b"{")
                self.server.stalled.set()
                while not self.server.resume.wait(0.05):
                    if mode == "trickle":
                        self.wfile.write(b" ")
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        data = self.server.operation(header, self.rfile.read(header.get("size", 0)))
        if isinstance(data, bytes):
            response = json.dumps({"version": 1, "ok": True, "size": len(data), "sha256": header["sha256"]}).encode()
            self.wfile.write(struct.pack(">I", len(response)) + response + data)
            return
        if data is not None:
            response = json.dumps({"version": 1, "ok": True, **data}).encode()
            self.wfile.write(struct.pack(">I", len(response)) + response)


def wait_for_state(client, run_id, expected, card_state=None):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        try:
            rows = client.call("RunInvocationsRequest", runId=run_id, card=path("work")).records
            if rows and rows[-1].executionState == expected and (card_state is None or rows[-1].state == card_state):
                return rows[-1]
        except RpcFailure as error:
            if error.kind != "not_found":
                raise
        time.sleep(0.1)
    raise TimeoutError(f"invocation did not reach remote state {expected}")


@pytest.fixture
def remote_gateway(monkeypatch):
    with tempfile.TemporaryDirectory(prefix="tsg-remote-", dir="/tmp") as directory:
        endpoint = str(Path(directory) / "broker")
        monkeypatch.setenv("TSUGUMI_EXECUTION_BROKER", "unix:" + endpoint)
        monkeypatch.delenv("TSUGUMI_EXECUTION_BROKER_TOKEN_FILE", raising=False)
        with Gateway(endpoint) as gateway:
            server = threading.Thread(target=gateway.serve_forever, kwargs={"poll_interval": 0.02})
            server.start()
            try:
                yield gateway
            finally:
                gateway.resume.set()
                gateway.shutdown()
                server.join()


@pytest.fixture
def running_job(project, remote_gateway, core_factory, tmp_path):
    daemon = core_factory(mocked_builds=True)
    client = daemon.connect()
    worker_project(client, project, tmp_path, [], outputs=FILE_FBS, sandbox=True, executionService="cluster",
                   retainArtifact=True, artifactOutput="doc",
                   executionResources={"cpuMillis": 500, "memoryBytes": 256 << 20})
    run_id = start_run(client, "work", propagate=False)
    live = wait_for_state(client, run_id, RemoteExecutionState.running)
    job_id = text(live.executionJobId)
    assert remote_gateway.submit_count == 1
    try:
        yield remote_gateway, daemon, client, run_id, job_id
    finally:
        remote_gateway.resume.set()


@pytest.mark.parametrize("field,value,diagnostic", [
    ("worker_contract", None, "Rebuild the daemon with this Tsugumi revision"),
    ("worker_contract", 1, "Rebuild the daemon with this Tsugumi revision"),
    ("worker_contract", 99, "Rebuild the daemon with this Tsugumi revision"),
    ("executor", None, "Start the Tsugumi remote execution daemon"),
    ("executor", "direct", "Start the Tsugumi remote execution daemon"),
    ("server_id", "3" * 64, "server ID changed"),
    ("isolation", None, "does not provide PID namespace isolation"),
    ("resource_limits", False, "cannot enforce resource limits"),
    ("max_jobs", 0, "did not report a valid concurrent job limit"),
])
def test_incompatible_remote_gateway_prevents_upload(project, remote_gateway, core_factory, tmp_path, field, value, diagnostic):
    remote_gateway.probe_overrides[field] = value
    client = core_factory(mocked_builds=True).connect()
    worker_project(client, project, tmp_path, [], outputs=FILE_FBS, sandbox=True, executionService="cluster",
                   executionResources={"cpuMillis": 500, "memoryBytes": 256 << 20})
    run_id = start_run(client, "work", propagate=False)
    status = wait_run(client, run_id, timeout=10)
    assert status.state == RunState.failed
    assert diagnostic in text(status.failure.diagnostic)
    assert not remote_gateway.blobs and remote_gateway.submit_count == 0


def test_remote_job_without_resource_limits(project, remote_gateway, core_factory, tmp_path):
    remote_gateway.resource_limits = False
    client = core_factory(mocked_builds=True).connect()
    worker_project(client, project, tmp_path, [], outputs=FILE_FBS, sandbox=True, executionService="cluster")
    run_id = start_run(client, "work", propagate=False)
    live = wait_for_state(client, run_id, RemoteExecutionState.running)
    assert live.executionResources is None
    remote_gateway.finish(text(live.executionJobId))
    assert wait_run(client, run_id, timeout=10).state == RunState.succeeded


@pytest.mark.parametrize("fault", ["type", "escape", "oversize"])
def test_invalid_remote_results_do_not_publish_outputs(running_job, fault):
    """The core validates output bytes and result members before publishing an artifact."""
    gateway, _, client, run_id, job_id = running_job
    gateway.finish(job_id, result_fault=fault)
    assert wait_run(client, run_id, timeout=10).state == RunState.failed
    record = client.call("RunRecordRequest", runId=run_id, card=path("work")).record
    assert record.summary.state == CardState.failed and record.summary.failure
    assert not record.outputs
    assert not client.call("ArtifactListRequest", runId=run_id, card=path("work")).artifacts


def assert_cancelled(client, gateway, run_id, job_id):
    finished = wait_for_state(client, run_id, RemoteExecutionState.cancelled, CardState.cancelled)
    assert finished.state == CardState.cancelled
    assert text(finished.executionJobId) == job_id
    assert gateway.submit_count == 1


@pytest.mark.parametrize("outage", ["offline", "registration_replaced"])
def test_core_restart_reattaches_to_the_same_external_job(running_job, outage):
    """An unknown transport outcome remains pending, and cancellation survives reconnection."""
    gateway, daemon, client, run_id, job_id = running_job
    if outage == "offline":
        gateway.online = False
    else:
        gateway.registered_server = "3" * 64
    wait_for_state(client, run_id, RemoteExecutionState.disconnected)
    daemon.restart(crash=True)
    client = daemon.connect()
    assert client.call("RunStatusRequest", runId=run_id).state == RunState.failed
    pending = wait_for_state(client, run_id, RemoteExecutionState.disconnected)
    assert text(pending.executionJobId) == job_id

    client.call("RunCancelRequest", runId=run_id)
    gateway.online = True
    gateway.registered_server = gateway.server_id
    assert_cancelled(client, gateway, run_id, job_id)


@pytest.mark.parametrize("mode,operation,request_cancel", [
    ("blackhole", "cancel", True),
    ("trickle", "status", True),
    ("blackhole", "status", False),
])
def test_core_shutdown_releases_an_unresponsive_gateway(running_job, mode, operation, request_cancel):
    """SIGTERM stops waiting for the executor and keeps any explicit cancellation intent."""
    gateway, daemon, client, run_id, job_id = running_job
    gateway.stall_operation = operation
    gateway.stall = mode
    if request_cancel and operation == "cancel":
        client.call("RunCancelRequest", runId=run_id)
    assert gateway.stalled.wait(5)
    if request_cancel and operation == "status":
        client.call("RunCancelRequest", runId=run_id)
    daemon.proc.send_signal(signal.SIGTERM)
    assert daemon.proc.wait(timeout=5) == 0
    assert gateway.receipts[job_id]["state"] == "running"

    gateway.stall = None
    gateway.resume.set()
    daemon.restart()
    client = daemon.connect()
    if not request_cancel:
        pending = wait_for_state(client, run_id, RemoteExecutionState.running)
        assert text(pending.executionJobId) == job_id
        assert gateway.receipts[job_id]["state"] == "running"
        gateway.finish(job_id)
        recovered = wait_for_state(client, run_id, RemoteExecutionState.succeeded, CardState.succeeded)
        assert recovered.state == CardState.succeeded
        assert text(recovered.executionJobId) == job_id
        artifacts = client.call("ArtifactListRequest", runId=run_id, card=path("work")).artifacts
        assert len(artifacts) == 1
        saved = client.call("ArtifactReadRequest", artifactId=text(artifacts[0].artifactId), limitBytes=1024)
        assert bytes(saved.bytes) == b"retained after shutdown\n"
        log = client.call("LogRangeRequest", runId=run_id, card=path("work"), invocation=1, stream=LogStream.stdout)
        assert text(log.text) == "one recovered message\n"
        assert gateway.submit_count == 1
    else:
        assert_cancelled(client, gateway, run_id, job_id)
