"""Core RPC handles lost replies within a core lifetime and interrupts runs after restart."""

import hashlib
import json
import os
import secrets
import signal
import socketserver
import sqlite3
import struct
import tempfile
import threading
import time
from pathlib import Path

import flatbuffers
import pytest
from harness import remote_worker as R
from harness.fixtures import worker_project
from harness.scenario import Project
from projects import FILE_FBS, start_run, wait_invocation, wait_run, wait_run_notification
from Tsugumi.Data.CardState import CardState
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.LogStream import LogStream
from Tsugumi.Data.RemoteExecutionState import RemoteExecutionState
from Tsugumi.Data.RunState import RunState
from Tsugumi.RPC.NotificationPayload import NotificationPayload
from Tsugumi.RPC.RunEventValue import RunEventValue
from Tsugumi.Worker.WorkerMessage import WorkerMessageT
from Tsugumi.Worker.WorkerOutput import WorkerOutputT
from Tsugumi.Worker.WorkerPayload import WorkerPayload
from Tsugumi.Worker.WorkerResult import WorkerResultT
from tsugumi_client import RpcFailure, make, new_id, path, text


class Gateway(socketserver.ThreadingUnixStreamServer):
    """Double of hostd's local broker, not its TLS worker connection.

    Keep partial/lost replies at the core-facing boundary so these tests still
    exercise core deadlines, shutdown and core lifetime interruption. Real hostd/TLS
    authentication is covered by the broker RPC and remote integration suites.
    """
    # hostd's broker listens with a backlog of 64. socketserver's default of 5
    # refuses a burst of concurrent connections on macOS, where a full Unix
    # socket backlog fails connect() instead of waiting.
    request_queue_size = 64
    instance = "1" * 64
    server_id = "2" * 64
    system = "x86_64-linux"

    def __init__(self, endpoint):
        super().__init__(endpoint, Handler)
        self.online = True
        self.registered_server = self.server_id
        self.resource_limits = True
        self.catalog_resource_limits = True
        self.catalog_contract = 3
        self.probe_overrides = {}
        self.receipts = {}
        self.submit_count = 0
        self.download_count = 0
        self.released = set()
        self.cancelled = set()
        self.cancel_received = threading.Event()
        self.core_ids = set()
        self.grade = 1
        self.blobs = set()
        self.blob_contents = {}
        self.specs = {}
        self.catalog_system = self.system
        self.results = {}
        self.stall = None
        self.skip_stall = 0
        self.hold_cancel = False
        self.defer_submit = False
        self.submit_applied = threading.Event()
        self.stall_operation = R.RequestBody.StatusRequest
        self.stalled = threading.Event()
        self.stalls = 0
        self.stalls_expected = 1
        self.stall_lock = threading.Lock()
        self.resume = threading.Event()
        # A running job's JSONL log, served by status requests from the
        # core's cursor; hidden ones appear only once the job has stopped.
        self.live_logs = {}
        self.hidden_live_logs = False
        self.log_offsets = []

    def finish(self, job, *, result_fault=None, logs=None, log_truncated=False):
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
            "environment.json": json.dumps({
                "paths": ["/nix/store/00000000000000000000000000000000-mock"],
                "directories": ["out"], "built": True, "log_truncated": log_truncated,
            }).encode(),
            "logs.jsonl": b'{"stream":1,"text":"one recovered message\\n"}\n',
        }
        if logs is not None:
            files["logs.jsonl"] = logs
        if result_fault == "log":
            files["logs.jsonl"] = b'{"stream":1,"text":"message"} trailing data\n'
        entries = [{"size": len(data), "sha256": blob(data), "path": path} for path, data in files.items()]
        if result_fault == "escape":
            entries[0]["path"] = "files/out/../../escape"
        if result_fault == "size":
            entries[0]["size"] += 1
        if result_fault == "oversize":
            next(entry for entry in entries if entry["path"] == "logs.jsonl")["size"] = (128 << 20) + 1
        manifest = R.pack(R.ResultManifestT(outputs=[R.ResultFileT(**entry) for entry in entries],
            invocationResultSha256=blob(bytes(buffer.Output()))))
        self.receipts[job] = {**self.receipts[job], "state": "succeeded", "result_sha256": blob(manifest)}

    def operation(self, q):
        self.core_ids.add(R.string(q.coreLifeId))
        kind, body = q.bodyType, q.body
        caps = R.CapabilitiesT(serverId=self.registered_server, nativeSystem=self.system, systems=[self.system],
                               resourceLimits=self.resource_limits, maxJobs=8, workerContract=12)
        if kind == R.RequestBody.CatalogRequest:
            caps.resourceLimits = self.catalog_resource_limits
            caps.workerContract = self.catalog_contract
            caps.nativeSystem = self.catalog_system
            caps.systems = [self.catalog_system]
            return R.reply(R.ReplyBody.Catalog, R.CatalogT(services=[R.TargetT(id="cluster", available=True, capabilities=caps,
                grade=self.grade)]))
        if self.server_id != self.registered_server:
            return R.failure(R.ErrorCode.RegistrationChanged, "execution service was replaced")
        if kind == R.RequestBody.ProbeRequest:
            caps.instanceId = self.instance
            for key, value in self.probe_overrides.items():
                setattr(caps, key, value)
            if caps.workerContract != 12 or not caps.maxJobs:
                return R.failure(R.ErrorCode.Rejected, "invalid capabilities")
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
            self.download_count += 1
            data = self.results[R.string(body.digest)]
            return R.blob_chunk(body.digest, len(data), body.offset,
                                data[body.offset:body.offset + body.limit])
        job = R.string(q.jobId)
        if kind == R.RequestBody.ReleaseRequest:
            self.released.add(job)
            return R.reply(R.ReplyBody.Ack, R.AckT())
        if kind == R.RequestBody.SubmitRequest:
            self.submit_count += 1
            spec = R.JobSpecT.InitFromPackedBuf(bytes(body.spec))
            self.specs[job] = spec
            assert {R.string(d) for d in spec.inputBlobs} <= self.blobs
            assert R.string(spec.system) == self.system
            fingerprint = hashlib.sha256(bytes(body.spec)).hexdigest()
            receipt = self.receipts.setdefault(job, {"state": "running", "fingerprint": fingerprint})
            assert receipt["fingerprint"] == fingerprint
            self.submit_applied.set()
            return None  # the receipt exists, but its reply was lost
        if kind == R.RequestBody.CancelRequest:
            fingerprint = R.string(body.fingerprint)
            receipt = self.receipts.setdefault(job, {"state": "cancelled", "fingerprint": fingerprint})
            assert receipt["fingerprint"] == fingerprint
            if receipt["state"] not in {"succeeded", "failed", "cancelled"}:
                receipt["state"] = "cancelled"
            self.cancelled.add(job)
            self.cancel_received.set()
        else:
            assert kind == R.RequestBody.StatusRequest
        state = self.receipts.get(job, {"state": "missing"})
        status = R.JobStatusT(
            state=getattr(R.JobState, state["state"].capitalize()), fingerprint=state.get("fingerprint"),
            resultSha256=state.get("result_sha256"), diagnostic=state.get("diagnostic"),
            logTruncated=state.get("log_truncated", False))
        log = self.live_logs.get(job, b"")
        # Status and Cancel replies carry a slice of the log from the cursor.
        if body.logLimit and log and (
                not self.hidden_live_logs or state["state"] in {"succeeded", "failed", "cancelled"}):
            self.log_offsets.append(body.logOffset)
            status.logOffset = body.logOffset
            status.logSize = len(log)
            status.logBytes = list(log[body.logOffset:body.logOffset + min(body.logLimit, 7)])
        return R.reply(R.ReplyBody.JobStatus, status)



class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        try:
            q = R.read_broker_request(self.rfile)
        except EOFError:
            return
        if self.server.hold_cancel and q.bodyType == R.RequestBody.CancelRequest:
            self.server.resume.wait()
            return
        mode = self.server.stall
        if mode and q.bodyType == self.server.stall_operation and self.server.skip_stall:
            self.server.skip_stall -= 1
            mode = None
        if mode and q.bodyType == self.server.stall_operation:
            try:
                if q.bodyType == R.RequestBody.SubmitRequest and not self.server.defer_submit:
                    self.server.operation(q)
                if mode == "trickle":
                    self.wfile.write(struct.pack("<I", 8192) + b"\x00")
                with self.server.stall_lock:
                    self.server.stalls += 1
                    if self.server.stalls == self.server.stalls_expected:
                        self.server.stalled.set()
                if mode == "trickle":
                    # Keep sending a partial frame until the test releases it.
                    while not self.server.resume.wait(0.05):
                        self.wfile.write(b"\x00")
                else:
                    self.server.resume.wait()
                    if q.bodyType == R.RequestBody.CatalogRequest:
                        self.wfile.write(R.frame(self.server.operation(q)))
                    if q.bodyType == R.RequestBody.SubmitRequest and self.server.defer_submit:
                        self.server.operation(q)
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        data = self.server.operation(q)
        if data is not None:
            self.wfile.write(R.frame(data))


def wait_for_state(client, run_id, expected, card_state=None):
    return wait_invocation(
        client, run_id, "work",
        lambda row: row.executionState == expected and (card_state is None or row.state == card_state),
    )


@pytest.fixture
def remote_gateway(monkeypatch):
    with tempfile.TemporaryDirectory(prefix="tsugumi-remote-", dir=os.environ.get("TSUGUMI_SOCKET_DIR", "/tmp")) as directory:
        endpoint = str(Path(directory) / "broker")
        monkeypatch.setenv("TSUGUMI_EXECUTION_BROKER", "unix:" + endpoint)
        capability = secrets.token_hex(32)
        capability_file = Path(directory) / "capability"
        capability_file.write_text(capability)
        capability_file.chmod(0o600)
        monkeypatch.setenv("TSUGUMI_EXECUTION_BROKER_CAPABILITY_FILE", str(capability_file))
        with Gateway(endpoint) as gateway:
            os.symlink(endpoint, endpoint + ".c")
            server = threading.Thread(target=gateway.serve_forever, kwargs={"poll_interval": 0.02})
            server.start()
            try:
                yield gateway
            finally:
                gateway.resume.set()
                gateway.shutdown()
                server.join()


def test_stop_is_accepted_while_worker_selection_catalog_is_pending(project, remote_gateway, core_factory, tmp_path):
    client = core_factory(mocked_builds=True).connect()
    worker_project(client, project, tmp_path, [], outputs=FILE_FBS, sandbox={},
                   executionService="cluster")
    client.ensure_subscription()
    rev = client.call("ProjectInspectRequest").rev
    remote_gateway.stall_operation = R.RequestBody.CatalogRequest
    # Accept queries the catalog once to validate the run. Hold the next lookup
    # inside the accepted run's worker selection.
    remote_gateway.skip_stall = 1
    remote_gateway.stall = "blackhole"
    selection_catalog = remote_gateway.stalled
    try:
        start = client.send("RunRequest", make("RunRequest", baseRev=rev, opId=new_id(),
                                               origin="test", card=path("work"), propagate=False))
        accepted = client.wait_notification(
            lambda n: n.valueType == NotificationPayload.RunEvent
            and n.value.valueType == RunEventValue.RunAcceptedEvent,
            timeout=10,
        )
        run_id = text(accepted.value.runId)
        assert selection_catalog.wait(10), "worker selection never requested the catalog"
        assert text(client.wait_reply(start).run.runId) == run_id
        remote_gateway.stalled = threading.Event()
        # Worker selection holds the first Catalog. Block all six RPC readers as well.
        remote_gateway.stalls_expected = 7
        plans = [client.send("RunPlanRequest", make("RunPlanRequest", card=path("work"),
                                                   propagate=False)) for _ in range(6)]
        assert remote_gateway.stalled.wait(10), "RunPlan requests did not fill the read pool"
        stop = client.send("RunCancelRequest", make("RunCancelRequest", runId=run_id))
        client.wait_notification(
            lambda n: n.valueType == NotificationPayload.RunEvent
            and text(n.value.runId) == run_id
            and n.value.valueType == RunEventValue.RunStatusChanged
            and n.value.value.status.state == RunState.cancelling,
            timeout=5,
        )
        assert client.wait_reply(stop).state == RunState.cancelling
        assert wait_run_notification(client, run_id, timeout=5).state == RunState.cancelled
    finally:
        remote_gateway.resume.set()
    for plan in plans:
        client.wait_reply(plan)
    assert remote_gateway.submit_count == 0


@pytest.fixture
def running_job(project, remote_gateway, core_factory, tmp_path):
    daemon = core_factory(mocked_builds=True)
    client = daemon.connect()
    worker_project(client, project, tmp_path, [], outputs=FILE_FBS, executionService="cluster",
                   retainArtifact=True, artifactOutput="doc",
                   sandbox={"limits": {"cpu": 0.5, "memoryMiB": 256}})
    run_id = start_run(client, "work", propagate=False)
    live = wait_for_state(client, run_id, RemoteExecutionState.running)
    job_id = text(live.executionJobId)
    assert remote_gateway.submit_count == 1
    spec = remote_gateway.specs[job_id]
    assert spec.cpuMillis == 500 and spec.memoryBytes == 256 << 20
    try:
        yield remote_gateway, daemon, client, run_id, job_id
    finally:
        remote_gateway.resume.set()


@pytest.mark.parametrize("field,value", [
    ("workerContract", 0),
    ("workerContract", 1),
    ("serverId", "3" * 64),
    ("resourceLimits", False),
    ("maxJobs", 0),
])
def test_incompatible_remote_gateway_prevents_upload(project, remote_gateway, core_factory, tmp_path, field, value):
    remote_gateway.probe_overrides[field] = value
    client = core_factory(mocked_builds=True).connect()
    worker_project(client, project, tmp_path, [], outputs=FILE_FBS, executionService="cluster",
                   sandbox={"limits": {"cpu": 0.5, "memoryMiB": 256}})
    run_id = start_run(client, "work", propagate=False)
    status = wait_run(client, run_id)
    assert status.state == RunState.failed
    assert status.failure.kind == FailureKind.unsupported
    assert not remote_gateway.blobs and remote_gateway.submit_count == 0


@pytest.mark.parametrize("catalog_resources,live_resources", [(False, True), (True, False)])
def test_resource_limits_use_live_worker_not_catalog(project, remote_gateway, core_factory, tmp_path,
                                                     catalog_resources, live_resources):
    remote_gateway.catalog_resource_limits = catalog_resources
    if live_resources:
        remote_gateway.catalog_contract = 99
    remote_gateway.resource_limits = live_resources
    client = core_factory(mocked_builds=True).connect()
    worker_project(client, project, tmp_path, [], outputs=FILE_FBS, executionService="cluster",
                   sandbox={"limits": {"cpu": 0.5, "memoryMiB": 256}})
    run_id = start_run(client, "work", propagate=False)
    if live_resources:
        live = wait_for_state(client, run_id, RemoteExecutionState.running)
        remote_gateway.finish(text(live.executionJobId))
        assert wait_run(client, run_id).state == RunState.succeeded
    else:
        status = wait_run(client, run_id)
        assert status.state == RunState.failed
        assert status.failure.kind == FailureKind.unsupported
        assert remote_gateway.submit_count == 0


@pytest.mark.parametrize("requested_system", [None, "aarch64-linux"])
def test_target_comes_from_fresh_probe(project, remote_gateway, core_factory, tmp_path, requested_system):
    """Assignment evaluates for the worker's current system, even with a stale catalog."""
    remote_gateway.catalog_system = "x86_64-linux"
    remote_gateway.system = "aarch64-linux"
    remote_gateway.resource_limits = False
    client = core_factory(mocked_builds=True).connect()
    settings = {} if requested_system is None else {"system": requested_system}
    worker_project(client, project, tmp_path, [], outputs=FILE_FBS, sandbox={},
                   executionService="cluster", **settings)
    run_id = start_run(client, "work", propagate=False)
    live = wait_for_state(client, run_id, RemoteExecutionState.running)
    spec = remote_gateway.specs[text(live.executionJobId)]
    assert R.string(spec.system) == text(live.executionSystem) == "aarch64-linux"
    assert R.string(spec.derivation).endswith(".drv")
    invocation = json.loads(remote_gateway.blob_contents[R.string(spec.invocationSha256)])
    assert invocation["archive_sha256"] in [R.string(d) for d in spec.inputBlobs]
    assert invocation["call_sha256"] in [R.string(d) for d in spec.inputBlobs]
    assert remote_gateway.blob_contents[invocation["archive_sha256"]] == b"test Nix derivation archive"
    remote_gateway.finish(text(live.executionJobId))
    assert wait_run(client, run_id).state == RunState.succeeded


def test_evaluation_failure_prevents_transfer(project, remote_gateway, core_factory):
    """A target-system evaluation error is visible before any upload or submission."""
    client = core_factory(mocked_builds=True).connect()
    Project(project).card("work", {"type": "code", "language": "bash", "sandbox": {}, "executionService": "cluster"},
                          outputs=FILE_FBS, plan={"error": "package is unavailable on this platform"}).save(client)
    run_id = start_run(client, "work", propagate=False)
    status = wait_run(client, run_id)
    assert status.state == RunState.failed
    diagnostic = text(status.failure.diagnostic)
    assert remote_gateway.system in diagnostic and "package is unavailable on this platform" in diagnostic
    assert not remote_gateway.blobs and remote_gateway.submit_count == 0


def test_remote_job_without_resource_limits(project, remote_gateway, core_factory, tmp_path):
    remote_gateway.resource_limits = False
    client = core_factory(mocked_builds=True).connect()
    worker_project(client, project, tmp_path, [], outputs=FILE_FBS, sandbox={}, executionService="cluster")
    run_id = start_run(client, "work", propagate=False)
    live = wait_for_state(client, run_id, RemoteExecutionState.running)
    assert live.executionResources is None
    spec = remote_gateway.specs[text(live.executionJobId)]
    assert spec.cpuMillis == spec.memoryBytes == 0
    remote_gateway.finish(text(live.executionJobId))
    assert wait_run(client, run_id).state == RunState.succeeded


@pytest.mark.parametrize("fault", ["type", "escape", "size", "oversize", "log"])
def test_invalid_remote_results_do_not_publish_outputs(running_job, fault):
    """The core validates output bytes and result members before publishing an artifact."""
    gateway, _, client, run_id, job_id = running_job
    gateway.finish(job_id, result_fault=fault)
    assert wait_run(client, run_id).state == RunState.failed
    record = client.call("RunRecordRequest", runId=run_id, card=path("work")).record
    assert record.summary.state == CardState.failed and record.summary.failure
    assert not record.outputs
    assert not client.call("ArtifactListRequest", runId=run_id, card=path("work")).artifacts


def test_rolled_back_remote_journal_leaves_no_gap(running_job, project):
    gateway, _, client, run_id, job_id = running_job
    with sqlite3.connect(project / ".tsugumi" / "runs.sqlite") as db:
        db.execute("""
            CREATE TRIGGER reject_invocation BEFORE INSERT ON invocations
            BEGIN SELECT RAISE(ABORT, 'test invocation write failed'); END
        """)
    rolled_back = "remote journal entry that must roll back"
    gateway.finish(job_id, logs=(json.dumps({"stream": LogStream.journal,
                                            "text": rolled_back + "\n"}) + "\n").encode())
    assert wait_run(client, run_id).state == RunState.failed

    for scope in ({}, {"card": path("work")}, {"card": path("work"), "invocation": 1}):
        journal = client.call("LogRangeRequest", runId=run_id, stream=LogStream.journal, **scope)
        saved = text(journal.text)
        entries = [json.loads(line) for line in saved.splitlines()]
        assert rolled_back not in saved
        assert any(entry["event"] == "failed" for entry in entries)
        assert journal.sizeBytes == journal.nextOffset == len(saved.encode())


def test_large_remote_build_entry_supports_small_backward_reads(running_job):
    gateway, _, client, run_id, job_id = running_job
    # Put a multibyte character across the on-disk chunk boundary.
    content = ("x" * 65535 + "雪\n" + "é\n" * 410000 + "end\n").encode()
    gateway.finish(job_id, logs=(json.dumps({"stream": LogStream.build,
                                            "text": content.decode()}, ensure_ascii=False) + "\n").encode())
    assert wait_run(client, run_id).state == RunState.succeeded

    args = dict(runId=run_id, card=path("work"), invocation=1, stream=LogStream.build)
    page = client.call("LogRangeRequest", **args, tail=True, limitBytes=11)
    assert page.sizeBytes == len(content)
    for _ in range(3):
        assert page.text == content[page.offset:page.nextOffset]
        assert page.lineNumber == content[:page.offset].count(b"\n") + 1
        page = client.call("LogRangeRequest", **args, before=page.offset, limitBytes=11)
    boundary = client.call("LogRangeRequest", **args, offset=65535, limitBytes=4)
    assert boundary.text == "雪\n".encode()
    assert boundary.lineNumber == 1


def test_log_read_reports_corrupt_chunk_offsets(running_job, project):
    gateway, _, client, run_id, job_id = running_job
    gateway.finish(job_id, logs=(json.dumps({"stream": LogStream.build,
                                            "text": "x" * 70000}) + "\n").encode())
    assert wait_run(client, run_id).state == RunState.succeeded
    with sqlite3.connect(project / ".tsugumi" / "runs.sqlite") as db:
        db.execute("UPDATE logs SET inv_off=inv_off+10 WHERE run_id=? AND stream=? AND inv_off=65536",
                   (run_id, LogStream.build))

    with pytest.raises(RpcFailure) as error:
        client.call("LogRangeRequest", runId=run_id, card=path("work"), invocation=1,
                    stream=LogStream.build, offset=65540, limitBytes=16)
    assert error.value.kind == "persistence_failed"


def test_long_terminal_redraw_is_consistent_in_forward_and_reverse_pages(running_job):
    gateway, _, client, run_id, job_id = running_job
    raw = "first\na" + "x" * (3 << 20) + "\r雪\nend\n"
    gateway.finish(job_id, logs=(json.dumps({"stream": LogStream.build,
                                            "text": raw}, ensure_ascii=False) + "\n").encode())
    assert wait_run(client, run_id).state == RunState.succeeded

    args = dict(runId=run_id, card=path("work"), invocation=1, stream=LogStream.build)
    expected = "first\n雪" + "x" * (3 << 20) + "\nend\n"
    reads = dict(args, terminal=True, limitBytes=1 << 20)
    page = client.call("LogRangeRequest", **reads, tail=True)
    end = page.nextOffset
    assert page.lineNumber == 2 and not page.startsLine
    assert page.rawSizeBytes == len(raw.encode())
    assert end == page.sizeBytes == len(expected) * 4
    assert text(page.text) == expected[page.offset // 4:]
    while page.offset:
        previous = client.call("LogRangeRequest", **reads, before=page.offset)
        assert previous.nextOffset == page.offset
        assert text(previous.text) == expected[previous.offset // 4:previous.nextOffset // 4]
        page = previous
    while page.nextOffset < end:
        page = client.call("LogRangeRequest", **reads, offset=page.nextOffset)
        assert text(page.text) == expected[page.offset // 4:page.nextOffset // 4]

    offset = 0
    while offset < len(raw.encode()):
        original = client.call("LogRangeRequest", **args, offset=offset, limitBytes=1 << 20)
        assert original.text == raw.encode()[offset:original.nextOffset]
        assert original.nextOffset > offset
        offset = original.nextOffset


@pytest.mark.parametrize(("outage", "completed"), [
    ("offline", False),
    ("registration_replaced", True),
])
def test_core_restart_interrupts_the_external_job(running_job, outage, completed):
    """A new core never fetches old results, even after transport returns."""
    gateway, daemon, client, run_id, job_id = running_job
    if outage == "offline":
        gateway.online = False
    else:
        gateway.registered_server = "3" * 64
    wait_for_state(client, run_id, RemoteExecutionState.disconnected)
    if completed:
        gateway.finish(job_id)
    daemon.restart(crash=True)
    client = daemon.connect()
    assert client.call("RunStatusRequest", runId=run_id).state == RunState.failed
    interrupted = wait_for_state(client, run_id, RemoteExecutionState.interrupted, CardState.failed)
    assert interrupted.endedAt is not None
    assert text(interrupted.executionJobId) == job_id
    assert interrupted.failure.kind == FailureKind.interrupted
    gateway.online = True
    gateway.registered_server = gateway.server_id
    gateway.cancel_received.wait()
    assert job_id in gateway.cancelled
    assert job_id not in gateway.released
    assert gateway.receipts[job_id]["state"] == ("succeeded" if completed else "cancelled")
    assert len(gateway.core_ids) == 2
    record = client.call("RunRecordRequest", runId=run_id, card=path("work")).record
    assert not record.outputs and not record.display
    assert not client.call("ArtifactListRequest", runId=run_id, card=path("work")).artifacts
    assert gateway.submit_count == 1 and gateway.download_count == 0


def test_network_cut_within_same_core_life_resumes(running_job):
    gateway, _, client, run_id, job_id = running_job
    original = set(gateway.core_ids)
    gateway.online = False
    wait_for_state(client, run_id, RemoteExecutionState.disconnected)
    gateway.online = True
    gateway.finish(job_id)
    assert wait_run(client, run_id).state == RunState.succeeded
    assert gateway.submit_count == 1
    assert gateway.core_ids == original


@pytest.mark.parametrize("operation", [R.RequestBody.StatusRequest, R.RequestBody.SubmitRequest],
                         ids=["pending-status", "pending-submit"])
def test_stop_cancels_remote_job_before_the_pending_reply(project, remote_gateway, core_factory, tmp_path, operation):
    client = core_factory(mocked_builds=True).connect()
    worker_project(client, project, tmp_path, [], outputs=FILE_FBS, sandbox={},
                   executionService="cluster")
    if operation == R.RequestBody.StatusRequest:
        run_id = start_run(client, "work", propagate=False)
        wait_for_state(client, run_id, RemoteExecutionState.running)
    remote_gateway.stall_operation = operation
    remote_gateway.stall = "blackhole"
    try:
        if operation == R.RequestBody.SubmitRequest:
            run_id = start_run(client, "work", propagate=False)
        assert remote_gateway.stalled.wait(10), "remote job never reached the gate"
        assert client.call("RunCancelRequest", runId=run_id).state == RunState.cancelling
        assert remote_gateway.cancel_received.wait(5), "Cancel waited behind the gated reply"
        assert wait_run(client, run_id, timeout=5).state == RunState.cancelled
        assert len(remote_gateway.cancelled) == 1
        assert remote_gateway.submit_count == 1
    finally:
        remote_gateway.resume.set()


def test_stop_reaches_worker_before_submit_is_applied(project, remote_gateway, core_factory, tmp_path):
    client = core_factory(mocked_builds=True).connect()
    worker_project(client, project, tmp_path, [], outputs=FILE_FBS, sandbox={},
                   executionService="cluster")
    remote_gateway.stall_operation = R.RequestBody.SubmitRequest
    remote_gateway.stall = "blackhole"
    remote_gateway.defer_submit = True
    run_id = start_run(client, "work", propagate=False)
    try:
        assert remote_gateway.stalled.wait(10), "Submit did not reach its gate"
        assert client.call("RunCancelRequest", runId=run_id).state == RunState.cancelling
        assert remote_gateway.cancel_received.wait(5), "Cancel waited for Submit"
        assert wait_run(client, run_id, timeout=5).state == RunState.cancelled
        job, = remote_gateway.cancelled
        assert job not in remote_gateway.specs
    finally:
        remote_gateway.resume.set()
    assert remote_gateway.submit_applied.wait(5)
    assert remote_gateway.receipts[job]["state"] == "cancelled"


def test_stop_interrupts_a_pending_result_download(running_job):
    gateway, _, client, run_id, job_id = running_job
    gateway.stall_operation = R.RequestBody.GetBlobRequest
    gateway.stall = "blackhole"
    gateway.finish(job_id)
    try:
        assert gateway.stalled.wait(10), "the result download did not reach the gate"
        assert client.call("RunCancelRequest", runId=run_id).state == RunState.cancelling
        assert gateway.cancel_received.wait(5), "Cancel waited behind the result download"
        assert wait_run(client, run_id, timeout=5).state == RunState.cancelled
        assert gateway.receipts[job_id]["state"] == "succeeded"
        assert gateway.download_count == 0
    finally:
        gateway.resume.set()


@pytest.mark.parametrize("mode,operation,request_cancel", [
    ("blackhole", R.RequestBody.CancelRequest, True),
    ("trickle", R.RequestBody.StatusRequest, True),
    ("blackhole", R.RequestBody.StatusRequest, False),
])
def test_core_shutdown_releases_an_unresponsive_gateway(running_job, mode, operation, request_cancel):
    """SIGTERM stops waiting; the next core interrupts rather than importing."""
    gateway, daemon, client, run_id, job_id = running_job
    gateway.stall_operation = operation
    gateway.hold_cancel = request_cancel and operation == R.RequestBody.StatusRequest
    gateway.stall = mode
    if request_cancel and operation == R.RequestBody.CancelRequest:
        client.call("RunCancelRequest", runId=run_id)
    gateway.stalled.wait()
    if request_cancel and operation == R.RequestBody.StatusRequest:
        client.call("RunCancelRequest", runId=run_id)
    daemon.proc.send_signal(signal.SIGTERM)
    # This test requires shutdown to be bounded even while the broker is silent.
    assert daemon.proc.wait() == 0
    assert gateway.receipts[job_id]["state"] == "running"

    gateway.stall = None
    gateway.hold_cancel = False
    gateway.resume.set()
    daemon.restart()
    client = daemon.connect()
    interrupted = wait_for_state(client, run_id, RemoteExecutionState.interrupted, CardState.failed)
    assert interrupted.endedAt is not None
    assert text(interrupted.executionJobId) == job_id
    assert interrupted.failure.kind == FailureKind.interrupted
    assert gateway.download_count == 0 and gateway.submit_count == 1


def test_remote_truncation_metadata_survives_import_and_restart(running_job):
    gateway, daemon, client, run_id, job_id = running_job
    limit = 128 << 20
    logs = b'{"stream":1,"text":"remote output"}\n'
    gateway.receipts[job_id]["log_truncated"] = True
    client.wait_notification(
        lambda n: n.valueType == NotificationPayload.LogAvailable
        and text(n.value.runId) == run_id
        and n.value.card is not None and list(map(text, n.value.card.segments)) == ["work"]
        and n.value.invocation == 1 and n.value.stream == LogStream.unspecified
    )
    live = client.call("RunRecordRequest", runId=run_id, card=path("work"), invocation=1)
    assert live.record.summary.logTruncatedAtBytes == limit
    assert live.record.summary.state == CardState.running
    gateway.finish(job_id, logs=logs, log_truncated=True)
    assert wait_run(client, run_id).state == RunState.succeeded
    daemon.restart()
    client = daemon.connect()
    record = client.call("RunRecordRequest", runId=run_id, card=path("work"), invocation=1)
    assert record.record.summary.logTruncatedAtBytes == limit
    stdout = client.call("LogRangeRequest", runId=run_id, card=path("work"), invocation=1,
                         stream=LogStream.standard_output, tail=True)
    assert stdout.logTruncatedAtBytes == limit
    journal = client.call("LogRangeRequest", runId=run_id, card=path("work"), invocation=1,
                          stream=LogStream.journal)
    assert journal.logTruncatedAtBytes == limit
    assert text(stdout.text).strip() == "remote output"


def test_grade_condition_selects_and_records_registered_worker(project, remote_gateway, core_factory, tmp_path):
    remote_gateway.grade = 0x8000000000000000
    client = core_factory(mocked_builds=True).connect()
    worker_project(client, project, tmp_path, [], outputs=FILE_FBS, sandbox={},
                   workerCondition={"language": "simple", "expression": "x & 0x8000000000000000 != 0"})
    run_id = start_run(client, "work", propagate=False)
    live = wait_for_state(client, run_id, RemoteExecutionState.running)
    assert text(live.executionService) == "cluster"
    remote_gateway.finish(text(live.executionJobId))
    assert wait_run(client, run_id).state == RunState.succeeded
    rows = client.call("RunInvocationsRequest", runId=run_id, card=path("work")).records
    completed = next(row for row in rows if row.invocation == live.invocation)
    assert text(completed.executionService) == "cluster"


def test_grade_condition_without_registered_match_fails_before_submission(project, remote_gateway, core_factory, tmp_path):
    client = core_factory(mocked_builds=True).connect()
    worker_project(client, project, tmp_path, [], outputs=FILE_FBS, sandbox={},
                   workerCondition={"language": "simple", "expression": "x > 1"})
    plan = client.call("RunPlanRequest", card=path("work"), propagate=False)
    assert any(text(d.code) == "invalid_setting" for d in plan.diagnostics)
    with pytest.raises(RpcFailure):
        start_run(client, "work", propagate=False)
    assert remote_gateway.submit_count == 0


@pytest.mark.parametrize("run_budget,build_budget", [(None, None), (2, 3)])
def test_remote_separates_build_and_call_budgets(project, remote_gateway, core_factory, tmp_path, run_budget, build_budget):
    client = core_factory(mocked_builds=True).connect()
    worker_project(client, project, tmp_path, [], outputs=FILE_FBS, sandbox={}, executionService="cluster",
                   timeoutSeconds=run_budget, buildTimeoutSeconds=build_budget)
    run_id = start_run(client, "work", propagate=False)
    live = wait_for_state(client, run_id, RemoteExecutionState.running)
    spec = remote_gateway.specs[text(live.executionJobId)]
    invocation = json.loads(remote_gateway.blob_contents[text(spec.invocationSha256)])
    call = WorkerMessageT.InitFromPackedBuf(remote_gateway.blob_contents[invocation["call_sha256"]])
    assert call.value.deadlineMs == (run_budget * 1000 if run_budget is not None else None)
    assert (0 < spec.buildTimeoutMs <= build_budget * 1000) if build_budget else spec.buildTimeoutMs == 0
    # Spending more than the call budget in transport/queue polling must not
    # make the core fail a call which the worker has not yet completed.
    remote_gateway.finish(text(live.executionJobId))
    assert wait_run(client, run_id).state == RunState.succeeded


def log_entries(*entries):
    return b"".join((json.dumps({"stream": stream, "text": value}) + "\n").encode() for stream, value in entries)


def recorded_log(client, run_id, stream):
    return text(client.call("LogRangeRequest", runId=run_id, card=path("work"), invocation=1,
                            stream=stream).text) or ""


def wait_recorded_log(client, run_id, stream, expected, timeout=10):
    deadline = time.monotonic() + timeout
    while (recorded := recorded_log(client, run_id, stream)) != expected:
        assert time.monotonic() < deadline, f"recorded {recorded!r}, expected {expected!r}"
        time.sleep(0.05)


def test_running_remote_log_arrives_once_across_reconnects(running_job):
    """Logs arrive while the job runs; the result's log only adds what is new."""
    gateway, _, client, run_id, job_id = running_job
    first = log_entries((LogStream.standard_output, "first line\n"), (LogStream.build, "building\n"))
    gateway.live_logs[job_id] = first
    wait_recorded_log(client, run_id, LogStream.standard_output, "first line\n")
    wait_recorded_log(client, run_id, LogStream.build, "building\n")
    assert client.call("RunStatusRequest", runId=run_id).state == RunState.running

    gateway.online = False
    wait_for_state(client, run_id, RemoteExecutionState.disconnected)
    second = first + log_entries((LogStream.standard_output, "after reconnect\n"))
    gateway.live_logs[job_id] = second
    gateway.online = True
    wait_recorded_log(client, run_id, LogStream.standard_output, "first line\nafter reconnect\n")

    complete = second + log_entries((LogStream.standard_error, "only in the result\n"))
    gateway.finish(job_id, logs=complete)
    assert wait_run(client, run_id).state == RunState.succeeded
    assert recorded_log(client, run_id, LogStream.standard_output) == "first line\nafter reconnect\n"
    assert recorded_log(client, run_id, LogStream.build) == "building\n"
    assert recorded_log(client, run_id, LogStream.standard_error) == "only in the result\n"
    # The cursor only moves forward, in bounded slices, and never restarts.
    assert gateway.log_offsets == sorted(gateway.log_offsets)
    assert gateway.log_offsets[-1] <= len(complete)
    assert gateway.submit_count == 1


def test_stopped_remote_job_keeps_its_collected_log(running_job):
    gateway, _, client, run_id, job_id = running_job
    gateway.hidden_live_logs = True
    gateway.live_logs[job_id] = log_entries((LogStream.standard_output, "before cancel\n"))
    assert client.call("RunCancelRequest", runId=run_id).state == RunState.cancelling
    assert wait_run(client, run_id).state == RunState.cancelled
    assert recorded_log(client, run_id, LogStream.standard_output) == "before cancel\n"
    assert gateway.download_count == 0


def test_remote_result_must_extend_the_recorded_log(running_job):
    gateway, _, client, run_id, job_id = running_job
    gateway.live_logs[job_id] = log_entries((LogStream.standard_output, "recorded\n"))
    wait_recorded_log(client, run_id, LogStream.standard_output, "recorded\n")
    gateway.finish(job_id, logs=log_entries((LogStream.standard_output, "replaced\n")))
    assert wait_run(client, run_id).state == RunState.failed
    assert recorded_log(client, run_id, LogStream.standard_output) == "recorded\n"
    assert not client.call("RunRecordRequest", runId=run_id, card=path("work")).record.outputs
