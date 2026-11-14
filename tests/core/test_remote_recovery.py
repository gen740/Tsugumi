"""Core RPC reattaches by the persisted external job ID after a lost reply and crash."""

import hashlib
import json
import secrets
import signal
import socketserver
import struct
import subprocess
import tempfile
import threading
import time
import uuid
from pathlib import Path

import flatbuffers
import pytest
from harness import remote_worker as R
from harness.fixtures import worker_project
from harness.scenario import Project
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
    """Double of agentd's local broker, not its TLS worker connection.

    Keep partial/lost replies at the core-facing boundary so these tests still
    exercise core deadlines, shutdown and durable recovery. Real agentd/TLS
    authentication is covered by the broker RPC and remote integration suites.
    """
    instance = "1" * 64
    server_id = "2" * 64
    system = "x86_64-linux"

    def __init__(self, endpoint, capability):
        super().__init__(endpoint, Handler)
        self.capability = capability
        self.online = True
        self.registered_server = self.server_id
        self.resource_limits = True
        self.probe_overrides = {}
        self.receipts = {}
        self.submit_count = 0
        self.probe_count = 0
        self.blobs = set()
        self.blob_contents = {}
        self.specs = {}
        self.catalog_system = self.system
        self.results = {}
        self.stall = None
        self.stall_operation = R.RequestBody.StatusRequest
        self.stalled = threading.Event()
        self.resume = threading.Event()

    def finish(self, job, *, result_fault=None, logs=None):
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
        if logs is not None:
            files["logs.jsonl"] = logs
        entries = [{"size": len(data), "sha256": blob(data), "path": path} for path, data in files.items()]
        if result_fault == "escape":
            entries[0]["path"] = "files/out/../../escape"
        if result_fault == "oversize":
            next(entry for entry in entries if entry["path"] == "logs.jsonl")["size"] = (16 << 20) + 129
        manifest = R.pack(R.ResultManifestT(outputs=[R.ResultFileT(**entry) for entry in entries],
            invocationResultSha256=blob(bytes(buffer.Output()))))
        self.receipts[job] = {**self.receipts[job], "state": "succeeded", "result_sha256": blob(manifest)}

    def operation(self, q):
        kind, body = q.bodyType, q.body
        caps = R.CapabilitiesT(serverId=self.registered_server, nativeSystem=self.system, systems=[self.system],
                               resourceLimits=self.resource_limits, maxJobs=8, workerContract=4)
        if kind == R.RequestBody.CatalogRequest:
            caps.nativeSystem = self.catalog_system
            caps.systems = [self.catalog_system]
            return R.reply(R.ReplyBody.Catalog, R.CatalogT(services=[R.TargetT(id="cluster", available=True, capabilities=caps)]))
        assert R.string(q.serverId) == self.server_id
        if R.string(q.serverId) != self.registered_server:
            return R.failure(R.ErrorCode.RegistrationChanged, "execution service was replaced")
        if kind == R.RequestBody.ProbeRequest:
            self.probe_count += 1
            caps.instanceId = self.instance
            for key, value in self.probe_overrides.items():
                setattr(caps, key, value)
            return R.reply(R.ReplyBody.Capabilities, caps)
        assert R.string(q.instanceId) == self.instance
        if kind == R.RequestBody.PutBlobRequest:
            digest = R.string(body.digest)
            offset = body.offset
            part = bytes(body.bytes)
            previous = self.blob_contents.get(digest, b"")
            if offset == len(previous):
                previous += part
                self.blob_contents[digest] = previous
            else:
                assert previous[offset:offset + len(part)] == part
            if len(previous) == body.total:
                assert hashlib.sha256(previous).hexdigest() == digest
                self.blobs.add(digest)
            return R.reply(R.ReplyBody.Ack, R.AckT())
        if not self.online:
            return None
        if kind == R.RequestBody.GetBlobRequest:
            data = self.results[R.string(body.digest)]
            return R.reply(R.ReplyBody.BlobChunk, R.BlobChunkT(digest=body.digest, total=len(data), offset=body.offset,
                                                             bytes=data[body.offset:body.offset + body.limit]))
        job = R.string(q.jobId)
        if kind == R.RequestBody.ReleaseRequest:
            return R.reply(R.ReplyBody.Ack, R.AckT())
        if kind == R.RequestBody.SubmitRequest:
            self.submit_count += 1
            spec = R.JobSpecT.InitFromPackedBuf(bytes(body.spec))
            self.specs[job] = spec
            assert {R.string(d) for d in spec.inputBlobs} <= self.blobs
            assert R.string(spec.system) == self.system
            if self.resource_limits:
                assert spec.cpuMillis == 500 and spec.memoryBytes == 256 << 20
            else:
                assert spec.cpuMillis == 0 and spec.memoryBytes == 0
            self.receipts[job] = {"state": "running", "fingerprint": hashlib.sha256(bytes(body.spec)).hexdigest()}
            return None  # the receipt exists, but its reply was lost
        if kind == R.RequestBody.CancelRequest:
            receipt = self.receipts[job]
            assert receipt["fingerprint"] == R.string(body.fingerprint)
            receipt["state"] = "cancelled"
        else:
            assert kind == R.RequestBody.StatusRequest
        state = self.receipts.get(job, {"state": "missing"})
        return R.reply(R.ReplyBody.JobStatus, R.JobStatusT(
            state=getattr(R.JobState, state["state"].capitalize()), fingerprint=state.get("fingerprint"),
            resultSha256=state.get("result_sha256"), diagnostic=state.get("diagnostic")))



class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        try:
            q = R.read_broker_request(self.rfile, self.server.capability)
        except EOFError:
            return
        mode = self.server.stall
        if mode and q.bodyType == self.server.stall_operation:
            try:
                if mode == "trickle":
                    self.wfile.write(struct.pack("<I", 8192) + b"\x00")
                self.server.stalled.set()
                while not self.server.resume.wait(0.05):
                    if mode == "trickle":
                        self.wfile.write(b"\x00")
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        data = self.server.operation(q)
        if data is not None:
            self.wfile.write(R.frame(data))


def wait_for_state(client, run_id, expected, card_state=None, timeout=15):
    deadline = time.monotonic() + timeout
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
        capability = secrets.token_hex(32)
        capability_file = Path(directory) / "capability"
        capability_file.write_text(capability)
        capability_file.chmod(0o600)
        monkeypatch.setenv("TSUGUMI_EXECUTION_BROKER_CAPABILITY_FILE", str(capability_file))
        with Gateway(endpoint, capability) as gateway:
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
    ("workerContract", 1, "Rebuild the remote worker"),
    ("workerContract", 99, "Rebuild the remote worker"),
    ("serverId", "3" * 64, "server ID changed"),
    ("resourceLimits", False, "cannot enforce resource limits"),
    ("nativeSystem", "", "valid system or concurrent job limit"),
    ("systems", [], "valid system or concurrent job limit"),
    ("maxJobs", 0, "valid system or concurrent job limit"),
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
    assert remote_gateway.probe_count > 0
    assert not remote_gateway.blobs and remote_gateway.submit_count == 0



def test_target_comes_from_fresh_probe(project, remote_gateway, core_factory, tmp_path):
    """Assignment evaluates for the worker's current system, even with a stale catalog."""
    remote_gateway.system = "aarch64-linux"
    remote_gateway.resource_limits = False
    client = core_factory(mocked_builds=True).connect()
    worker_project(client, project, tmp_path, [], outputs=FILE_FBS, sandbox=True, executionService="cluster")
    run_id = start_run(client, "work", propagate=False)
    live = wait_for_state(client, run_id, RemoteExecutionState.running)
    spec = remote_gateway.specs[text(live.executionJobId)]
    assert R.string(spec.system) == text(live.executionSystem) == "aarch64-linux"
    assert remote_gateway.probe_count > 0
    assert R.string(spec.derivation).endswith(".drv")
    invocation = json.loads(remote_gateway.blob_contents[R.string(spec.invocationSha256)])
    assert invocation["archive_sha256"] in [R.string(d) for d in spec.inputBlobs]
    assert invocation["call_sha256"] in [R.string(d) for d in spec.inputBlobs]
    assert remote_gateway.blob_contents[invocation["archive_sha256"]] == b"test Nix derivation archive"
    remote_gateway.finish(text(live.executionJobId))
    assert wait_run(client, run_id, timeout=10).state == RunState.succeeded


def test_evaluation_failure_prevents_transfer(project, remote_gateway, core_factory):
    """A target-system evaluation error is visible before any upload or submission."""
    client = core_factory(mocked_builds=True).connect()
    Project(project).card("work", {"language": "bash", "sandbox": True, "executionService": "cluster"},
                          outputs=FILE_FBS, plan={"error": "package is unavailable on this platform"}).save(client)
    run_id = start_run(client, "work", propagate=False)
    status = wait_run(client, run_id, timeout=10)
    assert status.state == RunState.failed
    diagnostic = text(status.failure.diagnostic)
    assert remote_gateway.system in diagnostic and "package is unavailable on this platform" in diagnostic
    assert not remote_gateway.blobs and remote_gateway.submit_count == 0


@pytest.mark.environment
def test_each_system_evaluates_without_host_builds(project, remote_gateway, core_factory):
    """One real evaluator produces distinct system-specific derivations without building outputs."""
    remote_gateway.resource_limits = False
    client = core_factory().connect()
    graph = Project(project)
    nonce = uuid.uuid4().hex
    graph.card("work", {"language": "bash", "sandbox": True, "executionService": "cluster"},
               outputs=FILE_FBS, sources={"default.nix":
                   '{ pkgs, inputs, params }: { run = "echo ${pkgs.stdenv.hostPlatform.system} ' + nonce + '"; }'})
    graph.save(client)
    derivations = {}
    for system in ["aarch64-linux", "x86_64-linux", "aarch64-linux"]:
        remote_gateway.system = system
        run_id = start_run(client, "work", propagate=False)
        live = wait_for_state(client, run_id, RemoteExecutionState.running, timeout=120)
        job_id = text(live.executionJobId)
        spec = remote_gateway.specs[job_id]
        assert R.string(spec.system) == system
        assert not Path(R.string(spec.output)).exists()
        drv = json.loads(subprocess.check_output(["nix", "derivation", "show", R.string(spec.derivation)], text=True))
        # Read the actual .drv rather than trusting only the job's system label.
        assert drv[R.string(spec.derivation)]["system"] == system
        if system in derivations:
            assert derivations[system] == R.string(spec.derivation)
        derivations[system] = R.string(spec.derivation)
        remote_gateway.finish(job_id)
        assert wait_run(client, run_id, timeout=10).state == RunState.succeeded
    assert len(set(derivations.values())) == 2


@pytest.mark.environment
def test_real_target_evaluation_error_prevents_transfer(project, remote_gateway, core_factory):
    """A Card unsupported on the worker system fails at assignment, before transport."""
    remote_gateway.resource_limits = False
    client = core_factory().connect()
    Project(project).card("work", {"language": "bash", "sandbox": True, "executionService": "cluster"},
        outputs=FILE_FBS, sources={"default.nix":
            '{ pkgs, inputs, params }: throw "unavailable for ${pkgs.stdenv.hostPlatform.system}"'}).save(client)
    run_id = start_run(client, "work", propagate=False)
    status = wait_run(client, run_id, timeout=120)
    assert status.state == RunState.failed
    assert "unavailable for " + remote_gateway.system in text(status.failure.diagnostic)
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
    ("blackhole", R.RequestBody.CancelRequest, True),
    ("trickle", R.RequestBody.StatusRequest, True),
    ("blackhole", R.RequestBody.StatusRequest, False),
])
def test_core_shutdown_releases_an_unresponsive_gateway(running_job, mode, operation, request_cancel):
    """SIGTERM stops waiting for the executor and keeps any explicit cancellation intent."""
    gateway, daemon, client, run_id, job_id = running_job
    gateway.stall_operation = operation
    gateway.stall = mode
    if request_cancel and operation == R.RequestBody.CancelRequest:
        client.call("RunCancelRequest", runId=run_id)
    assert gateway.stalled.wait(5)
    if request_cancel and operation == R.RequestBody.StatusRequest:
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


def test_maximum_log_budget_including_truncation_notice_imports(running_job):
    """The core accepts a full 16 MiB budget plus its out-of-budget notice."""
    gateway, _, client, run_id, job_id = running_job
    limit = 16 << 20
    entry = b'{"stream":1,"text":"' + b'x' * (limit - 23) + b'"}\n'
    assert len(entry) == limit
    notice = json.dumps({"stream": 3, "text": f"\n[Log output truncated at the {limit}-byte limit.]\n"},
                        separators=(",", ":")).encode() + b"\n"
    gateway.finish(job_id, logs=entry + notice)
    assert wait_run(client, run_id, timeout=30).state == RunState.succeeded
    journal = text(client.call("LogRangeRequest", runId=run_id, stream=LogStream.journal).text)
    assert f"truncated at the {limit}-byte limit" in journal
