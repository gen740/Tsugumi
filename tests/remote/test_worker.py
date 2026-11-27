"""Black-box admission, scoping and core lifetimes against tsugumi-workerd.

The socket-free C++ suite covers journal races and protocol verification. This
suite runs via tests/ci-remote-worker.sh on Linux, outside the build sandbox.
"""
import concurrent.futures
import fcntl
import hashlib
import http.client
import json
import os
from pathlib import Path
import shutil
import signal
import ssl
import socket
import struct
import subprocess
import sys
import time
from uuid import UUID, uuid4

import pytest
from harness import remote_worker as R
from harness.host import Host, HostFailure
from Tsugumi.Host.WorkerDashboardOperation import WorkerDashboardOperation as DashboardOp
from Tsugumi.RemoteWorker.WorkerReady import WorkerReady
from Tsugumi.RemoteWorker.DisplayCredit import DisplayCreditT
from Tsugumi.RemoteWorker.JobStatus import JobStatus
from Tsugumi.RemoteWorker.Reply import Reply
from Tsugumi.RemoteWorker.ViewSendRequest import ViewSendRequestT
from harness import peer_tls as TLS
from harness.file_events import process_file, wait_for_exit
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.LogStream import LogStream
from Tsugumi.Worker.WorkerPort import WorkerPortT
from test_worker_loading import card_package
from Tsugumi.Worker.WorkerCall import WorkerCallT
from Tsugumi.Worker.ViewControlKind import ViewControlKind
from Tsugumi.Worker.ViewEventKind import ViewEventKind
from Tsugumi.Worker.ViewUuid import ViewUuidT
from Tsugumi.Worker.WorkerViewEvent import WorkerViewEventT
from Tsugumi.Worker.WorkerViewReply import WorkerViewReplyT
from Tsugumi.Worker.WorkerMessage import WorkerMessageT
from Tsugumi.Worker.WorkerPayload import WorkerPayload

pytestmark = pytest.mark.skipif(sys.platform != "linux", reason="remote workers require Linux")


class Worker:
    def __init__(self, root, binary, *, extra=(), env=None):
        self.root = root
        self.identity = TLS.identity(root)
        self.scope = "c" * 64
        self.core_life_id = "e" * 64
        self.port = 0
        self.state = root / "state"
        self.state.mkdir(mode=0o700)
        self.authorized = self.state / "authorized_keys"
        self.authorized.write_text(TLS.public_key(self.identity) + " # test host\n")
        self.authorized.chmod(0o600)
        self.command = [binary, "--state-dir", str(self.state),
                        "--port", str(self.port), "--max-jobs", "1", *extra]
        self.env = env
        self.process = None
        try:
            self.start()
            self.caps = self.call(R.RequestBody.ProbeRequest, R.ProbeRequestT(), scoped=False).body
            self.instance = self.call(R.RequestBody.ProbeRequest, R.ProbeRequestT()).body.instanceId
        except BaseException:
            self.close()
            raise

    def start(self):
        self.log = (self.root / "worker.log").open("ab")
        ready = self.state / "worker.ready"
        ready.unlink(missing_ok=True)
        self.process = subprocess.Popen(self.command, env=self.env, stdout=self.log, stderr=self.log)
        published = process_file(self.process, ready)
        if published is None:
            raise RuntimeError((self.root / "worker.log").read_text())
        self.port = WorkerReady.GetRootAs(published.read_bytes()).Port()
        # Reconnect scenarios deliberately keep the same real endpoint.
        self.command[self.command.index("--port") + 1] = str(self.port)

    def close(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
            self.process.wait()
        self.log.close()

    def restart(self):
        self.process.kill()
        self.process.wait()
        self.log.close()
        self.start()

    def journal(self, job):
        return self.state / "projects" / self.scope / "jobs" / job / "state"

    def wait_state(self, job, *states):
        path = self.journal(job)

        def read():
            try:
                record = R.JournalStateT.InitFromPackedBuf(path.read_bytes())
            except FileNotFoundError:
                return None
            if record.state in states:
                return record
            assert record.state not in (R.JobState.Failed, R.JobState.Cancelled,
                                           R.JobState.Succeeded, R.JobState.Interrupted,
                                           R.JobState.Released), record
            return None

        record = process_file(self.process, path, read)
        assert record is not None, (self.root / "worker.log").read_text()
        return record

    def call(self, kind, body, *, job=None, scoped=True, unix=False, identity=None,
             read_reply=lambda stream: R.read(stream).payload, **routing):
        fields = {}
        if scoped:
            fields.update(projectScope=self.scope, coreLifeId=self.core_life_id)
            if kind != R.RequestBody.ProbeRequest:
                fields["instanceId"] = self.instance
        fields.update(routing)
        if job:
            fields["jobId"] = job
        request = R.request(kind, body, **fields)
        peer = socket.socket(socket.AF_UNIX if unix else socket.AF_INET, socket.SOCK_STREAM)
        with peer:
            peer.connect(str(self.state / "worker.sock") if unix else ("127.0.0.1", self.port))
            context = TLS.client_context(identity or self.identity, self.state / "identity.pem")
            with context.wrap_socket(peer, server_hostname=None) as secure:
                secure.sendall(R.frame(request))
                with secure.makefile("rb") as stream:
                    return read_reply(stream)

    def dashboard(self, path, *, body=None, identity=None, headers=None):
        context = TLS.client_context(identity or self.identity, self.state / "identity.pem")
        with socket.create_connection(("127.0.0.1", self.port), timeout=10) as peer:
            with context.wrap_socket(peer, server_hostname=None) as secure:
                data = b"" if body is None else json.dumps(body).encode()
                method = "GET" if body is None else "POST"
                head = {"Host": "worker", "Connection": "close", **(headers or {})}
                if body is not None:
                    head.update({"Content-Length": len(data), "Content-Type": "application/json",
                                 "X-Tsugumi-Dashboard": "1"})
                request = f"{method} {path} HTTP/1.1\r\n"
                request += "".join(f"{key}: {value}\r\n" for key, value in head.items())
                secure.sendall(request.encode() + b"\r\n" + data)
                with http.client.HTTPResponse(secure) as response:
                    response.begin()
                    result = response.read().decode()
                    if response.getheader("Content-Type") == "application/json; charset=utf-8":
                        result = json.loads(result)
                    return response.status, result

    def dashboard_change(self, path, **values):
        status, challenge = self.dashboard("/dashboard/challenge")
        assert status == 200
        return self.dashboard(path, body={**challenge, **values})

    def upload(self, job, data):
        digest = hashlib.sha256(data).hexdigest()
        self.put_chunk(job, digest, len(data), 0, data)
        return digest

    def put_chunk(self, job, digest, total, offset, data):
        chunk = R.put_blob_chunk(digest, total, offset, data)
        response = self.call(R.RequestBody.PutBlobRequest, chunk, job=job)
        assert response.bodyType == R.ReplyBody.Ack, vars(response.body)

    def spec(self, digest):
        return R.pack(R.JobSpecT(
            system=self.caps.nativeSystem,
            derivation="/nix/store/00000000000000000000000000000000-test.drv",
            output="/nix/store/00000000000000000000000000000000-test", invocationSha256=digest,
            inputBlobs=[digest], logLimitBytes=8 << 20,
        ))


@pytest.fixture
def worker(tmp_path):
    binary = os.environ.get("TSUGUMI_REMOTE_TEST_DAEMON")
    if not binary:
        pytest.skip("set TSUGUMI_REMOTE_TEST_DAEMON to the packaged tsugumi-workerd")
    instance = Worker(tmp_path, binary)
    try:
        yield instance
    finally:
        instance.close()


def test_probe_authentication_unix_and_private_state(worker):
    stranger = TLS.identity(worker.root, "stranger")
    with pytest.raises((ssl.SSLError, OSError, EOFError)):
        worker.call(R.RequestBody.ProbeRequest, R.ProbeRequestT(), scoped=False, identity=stranger)
    local = worker.call(R.RequestBody.ProbeRequest, R.ProbeRequestT(), unix=True)
    assert local.body.instanceId == worker.instance
    before = (worker.caps.serverId, worker.instance)
    worker.restart()
    assert worker.call(R.RequestBody.ProbeRequest, R.ProbeRequestT()).body.instanceId == before[1]
    assert worker.call(R.RequestBody.ProbeRequest, R.ProbeRequestT(), scoped=False).body.serverId == before[0]

    worker.authorized.write_text("# No authorized hosts\n")
    with pytest.raises((ssl.SSLError, OSError, EOFError)):
        worker.call(R.RequestBody.ProbeRequest, R.ProbeRequestT(), scoped=False)

def test_chunks_scopes_hashes_and_limits(worker):
    job = "1" * 64
    data = b"binary\0input\xff" * 100
    digest = worker.upload(job, data)
    response = worker.call(R.RequestBody.GetBlobRequest, R.GetBlobRequestT(digest=digest, offset=3, limit=17), job=job)
    assert bytes(response.body.bytes) == data[3:20]
    wrong = worker.call(R.RequestBody.GetBlobRequest, R.GetBlobRequestT(digest=digest), job=job, projectScope="d" * 64)
    assert wrong.bodyType == R.ReplyBody.Failure
    bad = worker.call(R.RequestBody.PutBlobRequest, R.PutBlobRequestT(digest="a" * 64, total=2, bytes=b"xx"), job=job)
    assert bad.bodyType == R.ReplyBody.Failure
    large = worker.call(R.RequestBody.PutBlobRequest, R.PutBlobRequestT(digest=digest, total=(64 << 30) + 1, bytes=b"x"), job=job)
    assert large.bodyType == R.ReplyBody.Failure
    limit = worker.call(R.RequestBody.GetBlobRequest, R.GetBlobRequestT(digest=digest, limit=(1 << 20) + 1), job=job)
    assert limit.bodyType == R.ReplyBody.Failure


def test_upload_retry_checks_only_overlap_and_charges_only_new_bytes(worker):
    job = "9" * 64
    data = b"staged blob"
    digest = hashlib.sha256(data).hexdigest()

    def put(chunk):
        return worker.call(R.RequestBody.PutBlobRequest,
                           R.PutBlobRequestT(digest=digest, total=len(data),
                                             offset=0, bytes=chunk), job=job)

    assert put(data[:3]).bodyType == R.ReplyBody.Ack
    # A sparse file models an existing upload that uses the remaining budget.
    # Restart to rebuild usage from the journal.
    other = worker.journal(job).parent / "parts" / ("b" * 64)
    with other.open("wb") as file:
        file.truncate((64 << 30) - len(data))
    worker.restart()
    assert put(b"bad").bodyType == R.ReplyBody.Failure
    assert put(data).bodyType == R.ReplyBody.Ack
    assert put(data).bodyType == R.ReplyBody.Ack
    response = worker.call(R.RequestBody.GetBlobRequest,
                           R.GetBlobRequestT(digest=digest, limit=len(data)), job=job)
    assert bytes(response.body.bytes) == data


def test_cancel_before_submit_and_release_tombstone(worker):
    job = "2" * 64
    digest = worker.upload(job, b"input")
    spec = worker.spec(digest)
    fingerprint = hashlib.sha256(spec).hexdigest()
    cancelled = worker.call(R.RequestBody.CancelRequest, R.CancelRequestT(fingerprint=fingerprint), job=job)
    assert cancelled.body.state == R.JobState.Cancelled
    submitted = worker.call(R.RequestBody.SubmitRequest, R.SubmitRequestT(spec=spec), job=job)
    assert submitted.body.state == R.JobState.Cancelled
    status = worker.call(R.RequestBody.StatusRequest, R.StatusRequestT(), job=job)
    assert status.body.state == R.JobState.Cancelled
    released = worker.call(R.RequestBody.ReleaseRequest, R.ReleaseRequestT(fingerprint=fingerprint), job=job)
    assert released.bodyType == R.ReplyBody.Ack
    worker.restart()
    late = worker.call(R.RequestBody.SubmitRequest, R.SubmitRequestT(spec=spec), job=job)
    assert late.body.state == R.JobState.Released
    missing = worker.call(R.RequestBody.GetBlobRequest, R.GetBlobRequestT(digest=digest), job=job)
    assert missing.bodyType == R.ReplyBody.Failure and missing.body.code == R.ErrorCode.NotFound


def test_concurrent_submit_has_one_fingerprint(worker):
    job = "3" * 64
    digest = worker.upload(job, b"not an invocation")
    spec = worker.spec(digest)
    def submit(_):
        return worker.call(R.RequestBody.SubmitRequest, R.SubmitRequestT(spec=spec), job=job)
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        replies = list(pool.map(submit, range(2)))
    assert all(r.bodyType == R.ReplyBody.JobStatus and R.string(r.body.fingerprint) == hashlib.sha256(spec).hexdigest() for r in replies)
    changed = R.JobSpecT.InitFromPackedBuf(spec)
    changed.logLimitBytes *= 2
    mismatch = worker.call(R.RequestBody.SubmitRequest, R.SubmitRequestT(spec=R.pack(changed)), job=job)
    assert mismatch.bodyType == R.ReplyBody.Failure and mismatch.body.code == R.ErrorCode.Rejected


def test_rejects_unsafe_options_before_listening(tmp_path):
    binary = os.environ.get("TSUGUMI_REMOTE_TEST_DAEMON")
    if not binary:
        pytest.skip("needs packaged worker")
    for extra in (["--host", "not-an-IP"], ["--max-jobs", "0"], ["--state-dir", "relative"]):
        result = subprocess.run([binary, "--state-dir", str(tmp_path / "state"), *extra],
                                capture_output=True, check=False)
        assert result.returncode != 0
    state = tmp_path / "unsafe-state"
    state.mkdir(mode=0o700)
    pem = TLS.identity(state, "identity")
    pem.chmod(0o644)
    result = subprocess.run([binary, "--state-dir", str(state), "--public-key"], capture_output=True, check=False)
    assert result.returncode != 0


def card_spec(peer, job, body, *, deadline_ms=None, outputs=(), inputs=(),
              output_budget=None):
    nix_directory = os.environ.get("TSUGUMI_NIX_DIR")
    if not nix_directory:
        pytest.skip("needs the installed worker build sources")
    expression = """
      { system, nixDirectory }:
      let
        pkgs = import (builtins.getFlake (toString ./.)).inputs.nixpkgs { inherit system; };
        worker = import (nixDirectory + "/worker/worker.nix") { inherit pkgs; };
        card = """ + card_package(body) + """;
      in pkgs.writeText "remote-card-plan" (builtins.toJSON {
        runs = [ "${card}" ];
        views = [];
        worker = "${worker}/bin/tsugumi-worker";
      })
    """
    derivation = subprocess.check_output(
        ["nix-instantiate", "--extra-experimental-features", "flakes",
         "--impure", "--expr", expression,
         "--argstr", "system", R.string(peer.caps.nativeSystem),
         "--argstr", "nixDirectory", nix_directory],
        text=True,
    ).strip()
    output = subprocess.check_output(
        ["nix-store", "--query", "--outputs", derivation], text=True,
    ).strip()
    closure = subprocess.check_output(
        ["nix-store", "--query", "--requisites", derivation], text=True,
    ).splitlines()
    archive = peer.root / "card.archive"
    with archive.open("wb") as stream:
        subprocess.run(["nix-store", "--export", *closure], stdout=stream, check=True)
    with archive.open("rb") as stream:
        archive_digest = hashlib.file_digest(stream, "sha256").hexdigest()
        stream.seek(0)
        total = archive.stat().st_size
        for offset in range(0, total, 1 << 20):
            peer.put_chunk(job, archive_digest, total, offset, stream.read(1 << 20))

    call = WorkerCallT(
        inputs=list(inputs), outputs=list(outputs), deadlineMs=deadline_ms,
    )
    message = WorkerMessageT(callId=1, valueType=WorkerPayload.WorkerCall, value=call)
    call_digest = peer.upload(job, R.pack(message, b"TSW1"))
    invocation = peer.upload(job, R.pack(R.InvocationT(
        callSha256=call_digest, archiveSha256=archive_digest,
        implementationIndex=1,
    )))
    return R.pack(R.JobSpecT(
        system=peer.caps.nativeSystem, derivation=derivation, output=output,
        invocationSha256=invocation, inputBlobs=[archive_digest, call_digest, invocation],
        logLimitBytes=8 << 20, **(output_budget or {}),
    ))


def test_global_cap_reconnects_within_one_core_life_and_enforces_call_budget(tmp_path):
    binary = os.environ.get("TSUGUMI_REMOTE_TEST_RAW_WORKER")
    if not binary:
        pytest.skip("needs the raw C++ executable for a controlled namespace launcher")
    tools = tmp_path / "tools"
    tools.mkdir()
    launcher = tools / "unshare"
    gate = tmp_path / "hold"
    os.mkfifo(gate)
    blocker, next_job, deadline_job = [str(n) * 64 for n in (4, 5, 6)]
    # The first two adapters hold their slots on a FIFO until cancelled. The
    # deadline job uses the real namespace launcher and executes a native Card.
    launcher.write_text('''#!/bin/sh
for arg do
    case "$arg" in
        --check-namespace) exit 0 ;;
        */"$TEST_DEADLINE_JOB"/work) exec "$TEST_UNSHARE" "$@" ;;
    esac
done
read -r line < "$TEST_GATE"
''')
    launcher.chmod(0o755)
    unshare = shutil.which("unshare")
    assert unshare, "the deadline job requires the Linux namespace launcher"
    peer = Worker(tmp_path, binary, env={**os.environ, "PATH": str(tools) + os.pathsep + os.environ["PATH"],
        "TEST_GATE": str(gate), "TEST_DEADLINE_JOB": deadline_job, "TEST_UNSHARE": unshare})
    fingerprints = {}

    def submit(job, spec):
        fingerprints[job] = hashlib.sha256(spec).hexdigest()
        accepted = peer.call(R.RequestBody.SubmitRequest, R.SubmitRequestT(spec=spec), job=job)
        assert accepted.bodyType == R.ReplyBody.JobStatus, vars(accepted.body)
        assert accepted.body.state == R.JobState.Queued

    def state(job):
        return peer.call(R.RequestBody.StatusRequest, R.StatusRequestT(), job=job).body.state

    def cancel(job):
        reply = peer.call(R.RequestBody.CancelRequest, R.CancelRequestT(fingerprint=fingerprints[job]), job=job)
        assert reply.bodyType == R.ReplyBody.JobStatus, vars(reply.body)

    try:
        corrupt = peer.journal("f" * 64)
        corrupt.parent.mkdir()
        corrupt.write_bytes(b"corrupt journal record")
        submit(blocker, peer.spec(peer.upload(blocker, b"staged input")))
        peer.wait_state(blocker, R.JobState.Running)
        submit(next_job, peer.spec(peer.upload(next_job, b"next input")))
        assert state(next_job) == R.JobState.Queued
        remove = "/dashboard/jobs/remove"
        status, refusal = peer.dashboard_change(remove, jobs=[f"{peer.scope}/{blocker}"], active=True)
        assert status == 200 and refusal["results"][0]["status"] == "supervised"
        assert state(blocker) == R.JobState.Running
        status, refusal = peer.dashboard_change(remove, jobs=[f"{peer.scope}/{next_job}"], active=False)
        assert status == 200 and refusal["results"][0]["status"] == "active"
        discard = "7" * 64
        submit(discard, peer.spec(peer.upload(discard, b"queued data to discard")))
        assert state(discard) == R.JobState.Queued
        status, removed = peer.dashboard_change(remove, jobs=[f"{peer.scope}/{discard}"], active=True)
        assert status == 200 and removed["results"][0]["status"] == "removed"
        assert state(discard) == R.JobState.Released
        peer.restart()
        assert state(blocker) == R.JobState.Running and state(next_job) == R.JobState.Queued
        cancel(blocker)
        peer.wait_state(blocker, R.JobState.Cancelled)
        peer.wait_state(next_job, R.JobState.Running)
        budget_ms = 1000
        submit(deadline_job, card_spec(peer, deadline_job, r"""
            puts("deadline-card-started");
            fflush(stdout);
            while (!call.effects->cancelled(call.host) &&
                   call.effects->remaining_ms(call.host) != 0)
              usleep(1000);
        """, deadline_ms=budget_ms))
        # A job held in the queue emits no event when its prospective Card
        # budget elapses. Keep the slot occupied for at least one such budget.
        time.sleep(budget_ms / 1000)
        assert state(deadline_job) == R.JobState.Queued
        cancel(next_job)
        # Succeeded means the supervisor imported a result. The Card outcome
        # is carried inside that result as a WorkerFailure.
        record = peer.wait_state(deadline_job, R.JobState.Succeeded)

        def result_bytes(digest):
            response = peer.call(R.RequestBody.GetBlobRequest,
                                 R.GetBlobRequestT(digest=digest), job=deadline_job)
            assert response.bodyType == R.ReplyBody.BlobChunk
            return bytes(response.body.bytes)

        manifest = R.ResultManifestT.InitFromPackedBuf(result_bytes(R.string(record.result)))
        files = {R.string(file.path): R.string(file.sha256) for file in manifest.outputs}
        assert R.ResultEnvironmentT.InitFromPackedBuf(result_bytes(files["environment"])).built
        logs = R.read_logs(result_bytes(files["logs"]))
        assert any(stream == LogStream.standard_output and text.strip() == "deadline-card-started"
                   for stream, text in logs)
        result = WorkerMessageT.InitFromPackedBuf(result_bytes(R.string(manifest.invocationResultSha256)))
        assert result.valueType == WorkerPayload.WorkerFailure
        assert result.value.failure.kind == FailureKind.timeout, R.string(result.value.failure.diagnostic)
    finally:
        try:
            for job in fingerprints:
                cancel(job)
            for job in fingerprints:
                peer.wait_state(job, R.JobState.Cancelled, R.JobState.Succeeded,
                                R.JobState.Failed, R.JobState.Interrupted, R.JobState.Released)
        finally:
            peer.close()


def test_remote_view_control_replay_and_bidirectional_credits(worker):
    job = "8" * 64
    spec = card_spec(worker, job, """
      static uint8_t message[TSUGUMI_MESSAGE_MAX];
      tsugumi_route data_route{}, control_route{};
      tsugumi_view_session data{}, control{};
      if (call.effects->view_open(call.host, "data", TSUGUMI_VIEW_PANEL, 0, 0, 0,
                                  &data_route, &data) ||
          call.effects->view_open(call.host, "control", TSUGUMI_VIEW_WINDOW, 0, 640, 480,
                                  &control_route, &control)) return 1;
      for (uint8_t i = 1; i <= 3; ++i) {
        message[0] = i;
        if (call.effects->send(call.host, &data, message, sizeof(message)))
          return 2;
      }
      tsugumi_view_event event{};
      if (call.effects->recv(call.host, &event, 60000) || event.kind != 0 ||
          event.route.bytes[0] != control_route.bytes[0] ||
          event.session.bytes[0] != control.bytes[0] ||
          event.data.size != 1 || event.data.data[0] != 42) return 3;
      if (call.effects->send(call.host, &control, event.data.data, event.data.size))
        return 4;
      bool closed = false, received = false;
      for (int i = 0; i < 2; ++i) {
        if (call.effects->recv(call.host, &event, 60000)) return 5;
        if (!closed && event.kind == 1 && event.route.bytes[0] == control_route.bytes[0] &&
            event.data.size == 0) {
          closed = true;
        } else if (!received && event.kind == 0 && event.route.bytes[0] == data_route.bytes[0] &&
                   event.data.size == 1 && event.data.data[0] == 9) {
          received = true;
        } else {
          return 6;
        }
      }
      if (call.effects->recv(call.host, &event, 100) != 1) return 7;
    """)
    submitted = worker.call(R.RequestBody.SubmitRequest, R.SubmitRequestT(spec=spec), job=job)
    assert submitted.bodyType == R.ReplyBody.JobStatus, vars(submitted.body)
    sequence = 0
    messages = []
    credits = []
    deadline = time.monotonic() + 300

    def read_status(stream):
        # Sample each maximum-size payload without unpacking its entire vector.
        payload = R.read_message(stream).Payload()
        reply = Reply()
        reply.Init(payload.Bytes, payload.Pos)
        assert reply.BodyType() == R.ReplyBody.JobStatus
        body = reply.Body()
        status = JobStatus()
        status.Init(body.Bytes, body.Pos)
        return status

    def poll(view_reply=None):
        nonlocal sequence
        query = R.StatusRequestT(displayAfter=sequence, displayCredits=credits,
                                 viewReply=view_reply)
        status = worker.call(R.RequestBody.StatusRequest, query, job=job, read_reply=read_status)
        assert status.State() in (R.JobState.Queued, R.JobState.Running,
                                  R.JobState.Succeeded), status.Diagnostic()
        if status.DisplaySequence() > sequence:
            sequence = status.DisplaySequence()
            for index in range(status.DisplaysLength()):
                display = status.Displays(index)
                messages.append((display.Session().Bytes(0), display.BytesLength(), display.Bytes(0)))
        return status

    def poll_until(done, view_reply=None):
        while time.monotonic() < deadline:
            status = poll(view_reply)
            if done(status):
                return status
            time.sleep(0.05)
        pytest.fail(f"relay stalled after {messages}")

    data = ViewUuidT(bytes=[1] * 16)
    control = ViewUuidT(bytes=[2] * 16)
    data_route = ViewUuidT(bytes=[3] * 16)
    control_route = ViewUuidT(bytes=[4] * 16)
    fingerprint = hashlib.sha256(spec).hexdigest()
    try:
        first = poll_until(lambda status: status.ViewRequest()).ViewRequest()
        assert first.RequestId() == 1 and first.Kind() == ViewControlKind.Open
        assert first.View() == b"data"
        replay = poll().ViewRequest()
        assert replay.RequestId() == 1 and replay.View() == b"data"
        reply = WorkerViewReplyT(requestId=1, status=0, session=data, route=data_route)
        # Lose the first status outcome and retry the same reply, including while
        # the next request is outstanding. It must only release the first waiter.
        poll(reply)
        second = poll_until(lambda status: status.ViewRequest(), reply).ViewRequest()
        assert second.RequestId() == 2 and second.View() == b"control"
        assert (second.Mode(), second.Width(), second.Height()) == (1, 640, 480)
        reply = WorkerViewReplyT(requestId=2, status=0, session=control, route=control_route)
        maximum = 1 << 24
        poll_until(lambda status: len(messages) >= 2 and status.ViewInputReady(), reply)
        assert messages == [(1, maximum, 1), (1, maximum, 2)]

        def send_event(kind, payload=None, *, session=control, route=control_route):
            event = WorkerViewEventT(kind=kind, session=session, route=route,
                                     bytes=payload)
            sent = worker.call(R.RequestBody.ViewSendRequest,
                               ViewSendRequestT(event=event, offset=0), job=job)
            assert sent.bodyType == R.ReplyBody.Ack, vars(sent.body)

        send_event(ViewEventKind.Message, [42])
        # The reverse message and its echo cross while the data window is full.
        poll_until(lambda _: len(messages) >= 3)
        assert messages[2:] == [(2, 1, 42)]
        send_event(ViewEventKind.Close)
        send_event(ViewEventKind.Close)
        send_event(ViewEventKind.Message, [9], session=data, route=data_route)
        credits.append(DisplayCreditT(address=data, released=maximum))
        status = poll_until(lambda status: status.State() == R.JobState.Succeeded and not status.DisplayPending())
        assert messages[3:] == [(1, maximum, 3)]

        def result_bytes(digest):
            response = worker.call(R.RequestBody.GetBlobRequest,
                                   R.GetBlobRequestT(digest=R.string(digest), offset=0), job=job)
            assert response.bodyType == R.ReplyBody.BlobChunk
            return bytes(response.body.bytes)

        manifest = R.ResultManifestT.InitFromPackedBuf(result_bytes(status.ResultSha256()))
        result = WorkerMessageT.InitFromPackedBuf(result_bytes(manifest.invocationResultSha256))
        assert result.valueType == WorkerPayload.WorkerResult, vars(result.value)
    finally:
        worker.call(R.RequestBody.CancelRequest,
                    R.CancelRequestT(fingerprint=fingerprint), job=job)
        worker.wait_state(job, R.JobState.Cancelled, R.JobState.Succeeded,
                          R.JobState.Failed, R.JobState.Interrupted)
        worker.call(R.RequestBody.ReleaseRequest,
                    R.ReleaseRequestT(fingerprint=fingerprint), job=job)


@pytest.mark.parametrize("case", ["artifact", "symlink", "byte-budget", "entry-budget"])
def test_real_remote_artifact_boundary(worker, case):
    job = "9" * 64
    outputs = [WorkerPortT(name="doc", required=True)]
    inputs = []
    create = """
      tsugumi_artifact_id id;
      const auto directory = fs::path(create_artifact(call, id));
    """
    if case == "artifact":
        capability = uuid4()
        initializer = ",".join(str(byte) for byte in capability.bytes)
        contents = b"staged"
        body = create + """
          tsugumi_artifact_id capability{{CAPABILITY}};
          tsugumi_bytes dir;
          if (call.effects->artifact_open(call.host, &capability, &dir) != 0) return 1;
          const fs::path granted{std::string(reinterpret_cast<const char*>(dir.data), dir.size)};
          if (granted.filename() != "ARTIFACT_UUID" ||
              std::distance(fs::directory_iterator(granted.parent_path()),
                            fs::directory_iterator{}) != 1) return 2;
          capability.bytes[15] ^= 1;
          tsugumi_bytes unused;
          if (call.effects->artifact_open(call.host, &capability, &unused) == 0)
            return 3;
          std::string contents;
          const auto input = granted / "nested/input.txt";
          std::ifstream(input) >> contents;
          if (contents != "staged" ||
              std::ofstream(input, std::ios::app) ||
              chmod(input.c_str(), 0600) == 0) return 4;
          static const uint8_t first[] = {0, 0};
          static const uint8_t second[] = {0, 255, 128};
          tsugumi_route route{};
          tsugumi_view_session session{};
          if (call.effects->view_open(call.host, "data", TSUGUMI_VIEW_PANEL, 0, 0, 0,
                                      &route, &session) ||
              call.effects->send(call.host, &session, first, sizeof(first)) ||
              call.effects->send(call.host, &session, second, sizeof(second))) return 1;
          std::ofstream(directory / "report.txt") << "artifact";
          std::ofstream("unrelated") << "not an artifact";
          out.artifact(id);
        """
        body = body.replace("CAPABILITY", initializer).replace("ARTIFACT_UUID", str(capability))
    elif case == "symlink":
        body = create + """
          fs::create_symlink("/etc/passwd", directory / "link");
          out.artifact(id);
        """
    elif case == "byte-budget":
        body = create + """
          std::ofstream(directory / "huge").close();
          fs::resize_file(directory / "huge", (64ull << 30) + 1);
          out.artifact(id);
        """
    else:
        body = create + """
          for (int i = 0; i < 1000; ++i)
            std::ofstream(directory / std::to_string(i)).close();
          out.artifact(id);
        """
    spec = card_spec(worker, job, body, outputs=outputs, inputs=inputs)
    fingerprint = hashlib.sha256(spec).hexdigest()
    try:
        response = worker.call(R.RequestBody.SubmitRequest, R.SubmitRequestT(spec=spec), job=job)
        assert response.bodyType == R.ReplyBody.JobStatus, vars(response.body)
        displays = []
        display_after = 0
        released = {}
        view_reply = None
        session = bytes([1] * 16)
        route = ViewUuidT(bytes=[2] * 16)
        deadline = time.monotonic() + 300
        while True:
            # Advance past replayed displays and return credit for consumed bytes.
            query = R.StatusRequestT(displayAfter=display_after, displayCredits=[
                DisplayCreditT(address=ViewUuidT(bytes=list(view)), released=total)
                for view, total in released.items()
            ], viewReply=view_reply)
            response = worker.call(R.RequestBody.StatusRequest, query, job=job)
            assert response.bodyType == R.ReplyBody.JobStatus, vars(response.body)
            status = response.body
            if status.artifactWanted:
                wanted = R.string(status.artifactWanted)
                found = wanted == str(capability)
                files = []
                if found:
                    files.append(R.InvocationFileT(
                        path="nested/input.txt", sha256=worker.upload(job, contents),
                        size=len(contents)))
                response = worker.call(R.RequestBody.JobArtifactRequest,
                    R.JobArtifactRequestT(id=wanted, fingerprint=fingerprint, found=found,
                                         files=files), job=job)
                assert response.bodyType == R.ReplyBody.Ack, vars(response.body)
            if status.viewRequest:
                assert status.viewRequest.requestId == 1 and status.viewRequest.view == b"data"
                view_reply = WorkerViewReplyT(requestId=1, status=0,
                                              session=ViewUuidT(bytes=list(session)),
                                              route=route)
            if status.displaySequence > display_after:
                display_after = status.displaySequence
                for message in status.displays:
                    view = bytes(message.session.bytes)
                    payload = bytes(message.bytes)
                    displays.append((view, payload))
                    released[view] = released.get(view, 0) + max(1, len(payload))
            if status.state == R.JobState.Succeeded and not status.displayPending:
                break
            assert status.state not in (R.JobState.Failed, R.JobState.Cancelled,
                                        R.JobState.Interrupted, R.JobState.Released), vars(status)
            assert time.monotonic() < deadline, vars(status)
            time.sleep(0.1)

        def result_bytes(digest):
            response = worker.call(R.RequestBody.GetBlobRequest,
                                   R.GetBlobRequestT(digest=digest), job=job)
            assert response.bodyType == R.ReplyBody.BlobChunk
            return bytes(response.body.bytes)

        manifest = R.ResultManifestT.InitFromPackedBuf(result_bytes(R.string(status.resultSha256)))
        entries = {R.string(entry.path): R.string(entry.sha256) for entry in manifest.outputs}
        assert all("display" not in name for name in entries)
        result = WorkerMessageT.InitFromPackedBuf(
            result_bytes(R.string(manifest.invocationResultSha256)))
        if case == "artifact":
            assert result.valueType == WorkerPayload.WorkerResult, vars(result.value)
            assert displays == [(session, b"\x00\x00"), (session, b"\x00\xff\x80")]
            created, = result.value.created
            output, = result.value.outputs
            assert output.bytes is not None
            assert UUID(bytes=bytes(created.id.bytes)).version == 4
            assert created.keep
            artifact_path = "files/artifacts/0/report.txt"
            assert {name for name in entries if name.startswith("files/")} == {artifact_path}
            assert result_bytes(entries[artifact_path]) == b"artifact"
        else:
            assert result.valueType == WorkerPayload.WorkerFailure
            assert result.value.failure.kind == FailureKind.output_invalid
            diagnostic = {
                "symlink": "symbolic link",
                "byte-budget": "output byte budget exceeded",
                "entry-budget": "output entry budget exceeded",
            }[case]
            assert diagnostic in R.string(result.value.failure.diagnostic)
            assert not any(name.startswith("files/") for name in entries)
    finally:
        worker.call(R.RequestBody.ReleaseRequest,
                    R.ReleaseRequestT(fingerprint=fingerprint), job=job)


def test_cancel_after_listener_restart_stops_child_and_reuses_slot(tmp_path):
    binary = os.environ.get("TSUGUMI_REMOTE_TEST_RAW_WORKER")
    if not binary:
        pytest.skip("needs the raw C++ executable for a controlled namespace launcher")
    tools = tmp_path / "tools"
    tools.mkdir()
    child_pid = tmp_path / "child.pid"
    gate = tmp_path / "hold"
    os.mkfifo(gate)
    launcher = tools / "unshare"
    entry = R.log_entries((LogStream.standard_output, "live entry\n"))
    later = R.log_entries((LogStream.standard_error, "after restart\n"))
    entry_file = tmp_path / "entry"
    entry_file.write_bytes(entry)

    def log_slice(job, offset, limit=1 << 20):
        reply = peer.call(R.RequestBody.StatusRequest, R.StatusRequestT(logOffset=offset, logLimit=limit), job=job)
        assert reply.bodyType == R.ReplyBody.JobStatus, vars(reply.body)
        return reply.body.logOffset, reply.body.logSize, bytes(reply.body.logBytes) if reply.body.logBytes is not None else b""

    # Like the invocation, append a flushed log entry to the job's log.
    launcher.write_text('#!/bin/sh\nfor arg do test "$arg" != --check-namespace || exit 0; done\n'
                        'for arg do case "$arg" in */work) mkdir -p "$arg/result" && '
                        'cat "$TEST_ENTRY" >> "$arg/result/logs";; esac; done\n'
                        'echo partial-log\necho $$ > "$TEST_CHILD_PID"\nread -r line < "$TEST_GATE"\n')
    launcher.chmod(0o755)
    peer = Worker(tmp_path, binary, env={**os.environ,
        "PATH": str(tools) + os.pathsep + os.environ["PATH"],
        "TEST_CHILD_PID": str(child_pid), "TEST_GATE": str(gate), "TEST_ENTRY": str(entry_file)})
    first, second = "7" * 64, "8" * 64
    directory = peer.state / "projects" / peer.scope / "jobs" / first

    def child(previous=None):
        try:
            pid = int(child_pid.read_text())
            return pid if pid != previous else None
        except (FileNotFoundError, ValueError):
            return None  # The launcher may still be writing its PID file.

    fingerprints = {}
    try:
        digest = peer.upload(first, b"orphan input")
        spec = peer.spec(digest)
        fingerprints[first] = hashlib.sha256(spec).hexdigest()
        accepted = peer.call(R.RequestBody.SubmitRequest, R.SubmitRequestT(spec=spec), job=first)
        assert accepted.body.state == R.JobState.Queued
        pid = process_file(peer.process, child_pid, child)
        assert pid is not None and Path(f"/proc/{pid}").exists()
        # The running job's log arrives in slices bounded by the worker.
        assert log_slice(first, 0, 5) == (0, len(entry), entry[:5])
        assert log_slice(first, 5) == (5, len(entry), entry[5:])
        # A removal by the external jobs command changes the in-memory
        # queue while the sole execution slot is still held by the first job.
        queued = "6" * 64
        queued_spec = peer.spec(peer.upload(queued, b"queued input"))
        accepted = peer.call(R.RequestBody.SubmitRequest, R.SubmitRequestT(spec=queued_spec), job=queued)
        assert accepted.body.state == R.JobState.Queued

        def wait_for_count(expected):
            deadline = time.monotonic() + 5
            while True:
                caps = peer.call(R.RequestBody.ProbeRequest, R.ProbeRequestT(), scoped=False).body
                if caps.activeJobs == expected:
                    return
                assert time.monotonic() < deadline, vars(caps)
                time.sleep(0.05)

        wait_for_count(2)
        subprocess.run([binary, "jobs", "remove", "--active",
                        f"{peer.scope}/{queued}", "--state-dir", str(peer.state)],
                       check=True, capture_output=True)
        wait_for_count(1)
        assert peer.wait_state(queued, R.JobState.Released).state == R.JobState.Released
        peer.restart()
        # A cursor survives the listener restart and returns only new bytes.
        cursor = len(entry)
        assert log_slice(first, cursor) == (cursor, cursor, b"")
        with (directory / "work" / "result" / "logs").open("ab") as log:
            log.write(later)
        assert log_slice(first, cursor) == (cursor, cursor + len(later), later)
        acknowledged = peer.call(R.RequestBody.CancelRequest,
                                 R.CancelRequestT(fingerprint=fingerprints[first]), job=first)
        assert acknowledged.body.state in (R.JobState.Cancelling, R.JobState.Cancelled)
        wait_for_exit(pid)
        peer.wait_state(first, R.JobState.Cancelled)
        cancelled = peer.call(R.RequestBody.StatusRequest, R.StatusRequestT(), job=first)
        assert cancelled.body.state == R.JobState.Cancelled
        blob = peer.call(R.RequestBody.GetBlobRequest, R.GetBlobRequestT(digest=digest), job=first)
        assert blob.bodyType == R.ReplyBody.BlobChunk and bytes(blob.body.bytes) == b"orphan input"
        assert "partial-log" in (directory / "work" / "adapter.log").read_text()
        # Cancellation keeps the log until release, for the same core life only.
        assert log_slice(first, 0) == (0, cursor + len(later), entry + later)
        with (directory / "work" / "result" / "logs").open("ab") as log:
            log.write(b"x" * (300 << 10))
        assert len(log_slice(first, 0)[2]) == 256 << 10
        # A new life cannot collect the old result, but can use the freed slot.
        peer.core_life_id = "f" * 64
        other = peer.call(R.RequestBody.StatusRequest, R.StatusRequestT(), job=first)
        assert other.bodyType == R.ReplyBody.Failure
        peer.call(R.RequestBody.ProbeRequest, R.ProbeRequestT())
        digest = peer.upload(second, b"next input")
        spec = peer.spec(digest)
        fingerprints[second] = hashlib.sha256(spec).hexdigest()
        peer.call(R.RequestBody.SubmitRequest, R.SubmitRequestT(spec=spec), job=second)
        peer.wait_state(second, R.JobState.Running)
        assert process_file(peer.process, child_pid, lambda: child(previous=pid)) is not None
        peer.call(R.RequestBody.CancelRequest,
                  R.CancelRequestT(fingerprint=fingerprints[second]), job=second)
        peer.wait_state(second, R.JobState.Cancelled)
        assert "partial-log" in (directory / "work" / "adapter.log").read_text()
    finally:
        try:
            if peer.process.poll() is not None:
                peer.start()
            for job, fingerprint in fingerprints.items():
                peer.call(R.RequestBody.CancelRequest, R.CancelRequestT(fingerprint=fingerprint), job=job)
                peer.wait_state(job, R.JobState.Failed, R.JobState.Cancelled)
        finally:
            peer.close()


def test_bounded_frames_and_single_listener(worker):
    for raw in (struct.pack("<I", (18 << 20) + 1), struct.pack("<I", 24) + bytes(24)):
        with socket.create_connection(("127.0.0.1", worker.port)) as connection:
            context = TLS.client_context(worker.identity, worker.state / "identity.pem")
            with context.wrap_socket(connection, server_hostname=None) as secure:
                secure.sendall(raw)
                with secure.makefile("rb") as stream:
                    error = R.read(stream).payload
            assert error.bodyType == R.ReplyBody.Failure and error.body.code == R.ErrorCode.Invalid
    command = list(worker.command)
    command[command.index("--port") + 1] = "0"
    command += ["--socket", str(worker.root / "duplicate.sock")]
    with subprocess.Popen(command, env=worker.env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) as duplicate:
        ready = worker.state / "worker.ready"

        def duplicate_ready():
            published = WorkerReady.GetRootAs(ready.read_bytes())
            return published if published.Pid() == duplicate.pid else None

        started = process_file(duplicate, ready, duplicate_ready)
        if started:
            duplicate.terminate()
        assert duplicate.wait() != 0 and started is None
    assert worker.call(R.RequestBody.ProbeRequest, R.ProbeRequestT()).body.instanceId == worker.instance


def test_limits_require_delegation(worker):
    job = "9" * 64
    digest = worker.upload(job, b"input")
    spec = R.JobSpecT.InitFromPackedBuf(worker.spec(digest))
    spec.cpuMillis = 500
    spec.memoryBytes = 256 << 20
    response = worker.call(R.RequestBody.SubmitRequest, R.SubmitRequestT(spec=R.pack(spec)), job=job)
    assert response.bodyType == R.ReplyBody.Failure and response.body.code == R.ErrorCode.Rejected


def test_card_secret_names_and_delivery_admission(worker):
    """A spec names its Card secrets only; their values are accepted once, for
    the job's fingerprint, while the job waits for them, and never kept."""
    job = "a" * 64
    digest = worker.upload(job, b"input")
    spec = R.JobSpecT.InitFromPackedBuf(worker.spec(digest))
    for names in (["Token"], ["token", "token"], ["x" * 65]):
        spec.secrets = names
        refused = worker.call(R.RequestBody.SubmitRequest, R.SubmitRequestT(spec=R.pack(spec)), job=job)
        assert refused.bodyType == R.ReplyBody.Failure and refused.body.code == R.ErrorCode.Invalid, names
    spec.secrets = ["token"]
    packed = R.pack(spec)
    fingerprint = hashlib.sha256(packed).hexdigest()
    accepted = worker.call(R.RequestBody.SubmitRequest, R.SubmitRequestT(spec=packed), job=job)
    assert accepted.bodyType == R.ReplyBody.JobStatus, vars(accepted.body)
    sentinel = "raw-card-secret-sentinel-0d4c9e"

    def deliver(fingerprint, name="token"):
        body = R.JobSecretsRequestT(fingerprint=fingerprint,
                                    secrets=[R.CardSecretT(name=name, value=sentinel)])
        return worker.call(R.RequestBody.JobSecretsRequest, body, job=job)

    # The fixture derivation never builds, so the job never waits for them.
    for refused in (deliver(fingerprint), deliver("0" * 64), deliver(fingerprint, "other")):
        assert refused.bodyType == R.ReplyBody.Failure and refused.body.code == R.ErrorCode.Rejected
    status = worker.call(R.RequestBody.StatusRequest, R.StatusRequestT(), job=job)
    assert not status.body.secretsWanted
    other = worker.call(R.RequestBody.JobSecretsRequest, R.JobSecretsRequestT(
        fingerprint=fingerprint, secrets=[R.CardSecretT(name="token", value=sentinel)]),
        job=job, coreLifeId="f" * 64)
    assert other.bodyType == R.ReplyBody.Failure and other.body.code == R.ErrorCode.Rejected
    worker.wait_state(job, R.JobState.Succeeded, R.JobState.Failed)
    worker.close()
    leaks = [str(f) for f in worker.root.rglob("*")
             if f.is_file() and not f.is_symlink() and sentinel.encode() in f.read_bytes()]
    assert not leaks


def test_listener_shutdown_unblocks_secret_delivery_to_a_stalled_reader(tmp_path):
    binary = os.environ.get("TSUGUMI_REMOTE_TEST_RAW_WORKER")
    if not binary:
        pytest.skip("needs the raw C++ executable for a controlled namespace launcher")
    tools = tmp_path / "tools"
    tools.mkdir()
    launcher = tools / "unshare"
    reading = tmp_path / "pipe-readable"
    launcher.write_text(f"""#!{sys.executable}
import fcntl, select, sys
from pathlib import Path

if "--check-namespace" in sys.argv:
    sys.exit(0)
work = Path(sys.argv[sys.argv.index("--invoke") + 1])
# Make the fixture independent of the system's default pipe capacity.
fcntl.fcntl(6, fcntl.F_SETPIPE_SZ, 4096)
(work / "secrets-wanted").touch()
if select.select([6], [], [], 20)[0]:
    Path({str(reading)!r}).touch()
# Retain the read end without consuming the frame until cancelled.
select.select([], [], [], 30)
""")
    launcher.chmod(0o755)
    peer = Worker(tmp_path, binary, env={**os.environ,
        "PATH": str(tools) + os.pathsep + os.environ["PATH"]})
    job = "a" * 64
    fingerprint = None

    def wait_for_marker(path):
        deadline = time.monotonic() + 10
        while not path.exists() and time.monotonic() < deadline:
            assert peer.process.poll() is None
            time.sleep(0.02)
        assert path.exists(), (peer.root / "worker.log").read_text()

    try:
        spec = R.JobSpecT.InitFromPackedBuf(peer.spec(peer.upload(job, b"input")))
        spec.secrets = ["token"]
        packed = R.pack(spec)
        accepted = peer.call(R.RequestBody.SubmitRequest, R.SubmitRequestT(spec=packed), job=job)
        assert accepted.bodyType == R.ReplyBody.JobStatus, vars(accepted.body)
        fingerprint = hashlib.sha256(packed).hexdigest()
        marker = peer.journal(job).parent / "work" / "secrets-wanted"
        wait_for_marker(marker)
        body = R.JobSecretsRequestT(fingerprint=fingerprint, secrets=[
            R.CardSecretT(name="token", value="x" * (1 << 17))])
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
            delivery = executor.submit(peer.call, R.RequestBody.JobSecretsRequest, body, job=job)
            try:
                wait_for_marker(reading)
                peer.process.terminate()
                assert peer.process.wait(timeout=5) == 0
                assert delivery.result(timeout=5).bodyType == R.ReplyBody.Failure
            finally:
                if peer.process.poll() is None:
                    peer.process.kill()
                    peer.process.wait()
    finally:
        try:
            if fingerprint:
                if peer.process.poll() is not None:
                    peer.log.close()
                    peer.start()
                peer.call(R.RequestBody.CancelRequest, R.CancelRequestT(fingerprint=fingerprint), job=job)
                peer.wait_state(job, R.JobState.Cancelled)
        finally:
            peer.close()


@pytest.fixture
def strace():
    path = shutil.which("strace")
    if not path:
        pytest.skip("needs strace (provided by tests/ci-remote-worker.sh)")
    return path


def test_idle_listener_does_no_io_on_released_jobs(worker, strace):
    worker.close()
    jobs = worker.journal("0" * 64).parent.parent
    tombstone = R.pack(R.JournalStateT(state=R.JobState.Released, fingerprint="a" * 64))
    for i in range(128):
        job = jobs / f"{i:064x}"
        job.mkdir(mode=0o700)
        (job / "state").write_bytes(tombstone)

    trace = worker.root / "idle.trace"
    # Trace the listener from launch while keeping its PID as the test's child.
    # Include fd paths so a directory sync would show even without a new open.
    worker.command[:0] = [
        strace, "-D", "-y", "-s", "4096", "-o", str(trace),
        "-e", "trace=%file,getdents64,fsync,fdatasync,?poll,ppoll",
    ]
    worker.start()
    offset = trace.stat().st_size

    deadline = time.monotonic() + 10
    while True:
        with trace.open() as output:
            output.seek(offset)
            idle = output.readlines()[1:]  # The offset can land mid-syscall.
        if sum("(Timeout)" in line for line in idle) >= 4:
            break
        assert worker.process.poll() is None and time.monotonic() < deadline, "".join(idle)
        time.sleep(0.05)
    assert not [line for line in idle if "/projects" in line], "".join(idle)
    assert all((job / "state").read_bytes() == tombstone for job in jobs.iterdir())


def test_startup_recovers_crash_during_cleanup(worker, strace):
    job = "9" * 64
    blob = worker.upload(job, b"remove after committing the tombstone")
    state_file = worker.journal(job)
    directory = state_file.parent
    worker.close()
    # jobs remove and Release use the same scratch erasure. Kill the remover
    # at the first scratch chmod, after its durable Released write.
    result = subprocess.run(
        [strace, "-o", str(worker.root / "cleanup.trace"),
         "-e", "trace=fchmodat",
         "-e", "inject=fchmodat:signal=SIGKILL",
         worker.command[0], "jobs", "remove", f"{worker.scope}/{job}",
         "--state-dir", str(worker.state)],
        capture_output=True, text=True, check=False,
    )
    assert result.returncode == -signal.SIGKILL, result.stderr
    assert (directory / "blobs" / blob).is_file()
    persisted = state_file.read_bytes()
    assert R.JournalStateT.InitFromPackedBuf(persisted).state == R.JobState.Released

    worker.start()
    assert set(directory.iterdir()) == {state_file}
    assert state_file.read_bytes() == persisted
    late = worker.call(R.RequestBody.PutBlobRequest, R.put_blob_chunk(blob, 0, 0, b""), job=job)
    assert late.bodyType == R.ReplyBody.Failure and late.body.code == R.ErrorCode.Rejected


def test_jobs_command_lists_and_removes_only_chosen_jobs(worker):
    binary = worker.command[0]

    def jobs(*args):
        return subprocess.run([binary, "jobs", *args, "--state-dir", str(worker.state)],
                              capture_output=True, text=True, check=False)

    def listed(*args):
        result = jobs("list", "--json", *args)
        assert result.returncode == 0, result.stderr
        return {row["job_id"]: row for row in json.loads(result.stdout)["jobs"]}

    staged, cancelled, released = "a" * 64, "b" * 64, "f" * 64
    worker.upload(staged, b"staged input")
    for job in (cancelled, released):
        spec = worker.spec(worker.upload(job, b"input"))
        fingerprint = hashlib.sha256(spec).hexdigest()
        worker.call(R.RequestBody.CancelRequest, R.CancelRequestT(fingerprint=fingerprint), job=job)
    worker.call(R.RequestBody.ReleaseRequest, R.ReleaseRequestT(fingerprint=fingerprint), job=released)
    # Queued and running records need a listener that does not act on them.
    worker.process.terminate()
    worker.process.wait()
    worker.log.close()
    project = worker.state / "projects" / worker.scope
    queued, running = "d" * 64, "e" * 64
    for job, state in ((queued, R.JobState.Queued), (running, R.JobState.Running)):
        (project / "jobs" / job).mkdir(mode=0o700)
        (project / "jobs" / job / "state").write_bytes(R.pack(R.JournalStateT(
            state=state, fingerprint="0" * 64, pid=4242, start=1, coreLifeId=worker.core_life_id)))
    lock = (project / "locks" / f"{running}.supervisor").open("w")
    fcntl.flock(lock, fcntl.LOCK_EX)

    queued_lock = project / "locks" / f"{queued}.supervisor"
    assert not queued_lock.exists()
    rows = listed()
    assert not queued_lock.exists(), "listing created a supervisor lock"
    assert set(rows) == {staged, cancelled, queued, running}
    assert rows[staged]["state"] == "uploading" and "never submitted" in rows[staged]["reason"]
    assert rows[cancelled]["state"] == "cancelled" and "not released" in rows[cancelled]["reason"]
    assert rows[running]["reason"] == "running (supervisor pid 4242)"
    assert all(row["project_scope"] == worker.scope and row["core_last_seen"] == "unknown"
               and row["core_life_id"] == worker.core_life_id and row["bytes"] > 0
               and row["last_written"].endswith("Z") for row in rows.values())
    assert listed("--all")[released]["state"] == "released"
    text = jobs("list")
    assert text.returncode == 0 and staged[:12] in text.stdout and released[:12] not in text.stdout

    def name(job):
        return f"{worker.scope[:8]}/{job[:8]}"

    assert jobs("remove", staged[:8]).returncode != 0
    assert jobs("remove", f"{worker.scope}/{'9' * 64}").returncode != 0
    result = jobs("remove", name(staged), name(cancelled))
    assert result.returncode == 0, result.stderr
    assert sorted(path.name for path in (project / "jobs" / staged).iterdir()) == ["state"]
    assert set(listed()) == {queued, running}
    assert listed("--all")[cancelled]["state"] == "released"

    refused = jobs("remove", name(queued))
    assert refused.returncode != 0 and "--active" in refused.stderr
    assert listed()[queued]["state"] == "queued"
    refused = jobs("remove", "--active", name(running))
    assert refused.returncode != 0 and "supervisor process is running it" in refused.stderr
    lock.close()
    assert "no supervisor runs it" in listed()[running]["reason"]
    result = jobs("remove", "--active", name(queued), name(running))
    assert result.returncode == 0, result.stderr
    assert listed() == {}
    assert jobs("list").stdout.startswith("No job data")

    # The tombstone still rejects the removed job's late transfer.
    worker.start()
    late = worker.call(R.RequestBody.PutBlobRequest, R.put_blob_chunk(hashlib.sha256(b"x").hexdigest(), 1, 0, b"x"), job=staged)
    assert late.bodyType == R.ReplyBody.Failure and late.body.code == R.ErrorCode.Rejected

def test_dashboard_authentication_challenges_and_key_updates(worker):
    stranger = TLS.identity(worker.root, "new-host")
    key = TLS.public_key(stranger)
    own_key = TLS.public_key(worker.identity)
    before = worker.authorized.read_bytes()
    status, html = worker.dashboard("/dashboard?lang=ja")
    assert status == 200 and '<html lang="ja">' in html
    status, keys = worker.dashboard("/dashboard/keys")
    assert status == 200 and keys == {"keys": [own_key], "current": own_key}

    add = "/dashboard/keys/add"
    drop = "/dashboard/keys/remove"
    challenges = "/dashboard/challenge"
    status, challenge = worker.dashboard(challenges)
    assert status == 200
    for path, body in (("/dashboard", None), (add, {**challenge, "key": key}),
                       (drop, {**challenge, "key": own_key})):
        with pytest.raises((ssl.SSLError, OSError, http.client.RemoteDisconnected)):
            worker.dashboard(path, body=body, identity=stranger)
    assert worker.authorized.read_bytes() == before

    assert worker.dashboard(add)[0] == 404
    assert worker.dashboard(add, body={"key": key})[0] in (400, 403)
    assert worker.dashboard(challenges, headers={"Origin": "https://example.invalid"})[0] == 403
    addition = {**challenge, "key": key}
    assert worker.dashboard(add, body=addition, headers={"Origin": "https://example.invalid"})[0] == 403
    assert worker.authorized.read_bytes() == before
    assert worker.dashboard(add, body=addition)[0] == 200
    assert worker.authorized.read_bytes().startswith(before)
    # A fresh handshake accepts the added key. Even another authorized host
    # cannot use a challenge issued to this peer.
    assert worker.dashboard("/dashboard", identity=stranger)[0] == 200
    status, challenge = worker.dashboard(challenges)
    assert status == 200
    deletion = {**challenge, "key": key}
    assert worker.dashboard(drop, body=deletion, identity=stranger)[0] == 403
    assert worker.dashboard(drop, body=deletion)[0] == 200
    assert worker.authorized.read_bytes() == before
    assert worker.dashboard(add, body=addition)[0] == 403
    with pytest.raises((ssl.SSLError, OSError, http.client.RemoteDisconnected)):
        worker.dashboard("/dashboard/keys", identity=stranger)


def test_dashboard_lists_and_releases_the_same_jobs_as_cli(worker):
    selected, cli, keep = (letter * 64 for letter in "abc")
    for job in (selected, cli, keep):
        worker.upload(job, b"staged input")
    command = [worker.command[0], "jobs"]
    listing = subprocess.check_output(command + ["list", "--json", "--state-dir", str(worker.state)])
    assert worker.dashboard("/dashboard/jobs") == (200, json.loads(listing))

    stranger = TLS.identity(worker.root, "stranger")
    status, challenge = worker.dashboard("/dashboard/challenge")
    assert status == 200
    path = "/dashboard/jobs/remove"
    body = {**challenge, "jobs": [f"{worker.scope}/{selected}"], "active": False}
    with pytest.raises((ssl.SSLError, OSError, http.client.RemoteDisconnected)):
        worker.dashboard(path, body=body, identity=stranger)
    assert worker.dashboard("/dashboard/jobs") == (200, json.loads(listing))

    assert worker.dashboard(path, body=body)[1]["results"][0]["status"] == "removed"
    subprocess.run(command + ["remove", "--state-dir", str(worker.state), f"{worker.scope}/{cli}"],
                   check=True, capture_output=True)
    assert list(worker.journal(selected).parent.iterdir()) == [worker.journal(selected)]
    assert worker.journal(selected).read_bytes() == worker.journal(cli).read_bytes()
    record = R.JournalStateT.InitFromPackedBuf(worker.journal(selected).read_bytes())
    assert record.state == R.JobState.Released
    status, data = worker.dashboard("/dashboard/jobs")
    assert status == 200 and [row["job_id"] for row in data["jobs"]] == [keep]
    late = worker.call(R.RequestBody.PutBlobRequest,
                       R.put_blob_chunk(hashlib.sha256(b"x").hexdigest(), 1, 0, b"x"), job=selected)
    assert late.bodyType == R.ReplyBody.Failure and late.body.code == R.ErrorCode.Rejected


def test_host_dashboard_requires_current_verified_registration(worker):
    host = Host(worker.root / "host")
    try:
        # Use the scratch host's public key and the worker pin from its probe.
        host_key = R.string(host.call("ExecutionServicesRequest").hostPublicKey)
        with worker.authorized.open("a") as keys:
            keys.write(host_key + "\n")
        worker_key = next(line.split("public key: ", 1)[1]
                          for line in (worker.root / "worker.log").read_text().splitlines()
                          if "public key: " in line)
        registration = dict(id="scratch-worker", name="Scratch worker",
                            endpoint=f"tls://127.0.0.1:{worker.port}", workerPublicKey=worker_key)

        def page():
            return host.call("WorkerDashboardRequest", id=registration["id"],
                             operation=DashboardOp.Page, language="en")

        host.call("ExecutionServicePutRequest", **registration, verify=False)
        with pytest.raises(HostFailure, match="Verify the execution service"):
            page()
        host.call("ExecutionServicePutRequest", **registration, verify=True)
        opened = page()
        assert '<html lang="en">' in R.string(opened.body)
        job = "a" * 64
        worker.upload(job, b"input staged on scratch worker")
        args = dict(id=registration["id"], generation=opened.generation)
        listed = host.call("WorkerDashboardRequest", **args, operation=DashboardOp.Jobs)
        assert json.loads(listed.body) == worker.dashboard("/dashboard/jobs")[1]
        token = json.loads(host.call("WorkerDashboardRequest", **args, operation=DashboardOp.Challenge).body)
        removed = host.call("WorkerDashboardRequest", **args, operation=DashboardOp.RemoveJobs,
                            body=json.dumps({**token, "jobs": [f"{worker.scope}/{job}"], "active": False}))
        assert json.loads(removed.body)["results"][0]["status"] == "removed"
        host.call("ExecutionServiceRemoveRequest", id=registration["id"])
        host.call("ExecutionServicePutRequest", **registration, verify=True)
        with pytest.raises(HostFailure, match="The execution service changed"):
            host.call("WorkerDashboardRequest", **args, operation=DashboardOp.Jobs)
    finally:
        host.close()
