"""Black-box admission, scoping and core lifetimes against tsugumi-workerd.

The socket-free C++ suite covers journal races and protocol verification. This
suite runs on the Linux CI runner, outside the build sandbox.
"""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import shutil
import ssl
import socket
import struct
import subprocess
import sys
import time

import pytest
from harness import remote_worker as R
from harness import peer_tls as TLS
from harness.file_events import process_file, wait_for_exit
from Tsugumi.Data.CardPath import CardPathT
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.FlatBufferData import FlatBufferDataT
from Tsugumi.Worker.EnvVar import EnvVarT
from Tsugumi.Worker.WorkerCall import WorkerCallT
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
        ready = self.state / "worker.ready.json"
        ready.unlink(missing_ok=True)
        self.process = subprocess.Popen(self.command, env=self.env, stdout=self.log, stderr=self.log)
        published = process_file(self.process, ready)
        if published is None:
            raise RuntimeError((self.root / "worker.log").read_text())
        self.port = json.loads(published.read_text())["port"]
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
                record = json.loads(path.read_text())
            except FileNotFoundError:
                return None
            if record["state"] in states:
                return record
            assert record["state"] not in (R.JobState.Failed, R.JobState.Cancelled,
                                           R.JobState.Succeeded, R.JobState.Interrupted,
                                           R.JobState.Released), record
            return None

        record = process_file(self.process, path, read)
        assert record is not None, (self.root / "worker.log").read_text()
        return record

    def call(self, kind, body, *, job=None, scoped=True, unix=False, identity=None, **routing):
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
                    return R.read(stream).payload

    def upload(self, job, data):
        digest = hashlib.sha256(data).hexdigest()
        self.put_chunk(job, digest, len(data), 0, data)
        return digest

    def put_chunk(self, job, digest, total, offset, data):
        chunk = R.PutBlobRequestT(digest=digest, total=total, offset=offset, bytes=data)
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


def test_cancel_before_submit_and_release_tombstone(worker):
    job = "2" * 64
    digest = worker.upload(job, b"input")
    spec = worker.spec(digest)
    fingerprint = hashlib.sha256(spec).hexdigest()
    cancelled = worker.call(R.RequestBody.CancelRequest, R.CancelRequestT(fingerprint=fingerprint), job=job)
    assert cancelled.body.state == R.JobState.Cancelled
    submitted = worker.call(R.RequestBody.SubmitRequest, R.SubmitRequestT(spec=spec), job=job)
    assert submitted.body.state == R.JobState.Cancelled
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


def deadline_spec(peer, job, budget_ms):
    # Use the runner's pinned tools to build the smallest Bash Card manifest.
    bash, mkfifo = shutil.which("bash"), shutil.which("mkfifo")
    assert bash and mkfifo, "the deadline Card requires Bash and mkfifo from tsugumi-test-env"
    worker = Path(os.environ["TSUGUMI_WORKER"])
    expression = r'''
      { system, bashRoot, coreutilsRoot, workerRoot }:
      let
        shell = builtins.storePath bashRoot + "/bin/bash";
      in
      builtins.derivation {
        name = "remote-test-card-deadline";
        inherit system;
        builder = shell;
        args = [ "-c" ''printf '%s' "$manifest" > "$out"'' ];
        manifest = builtins.toJSON {
          inherit shell;
          path = builtins.storePath coreutilsRoot + "/bin";
          run = {
            worker = builtins.storePath workerRoot + "/bin/tsugumi-worker";
            bash = ''mkfifo "$HOME/gate" && read -r line < "$HOME/gate"'';
          };
        };
      }
    '''
    derivation = subprocess.check_output(
        ["nix-instantiate", "--expr", expression,
         "--argstr", "system", R.string(peer.caps.nativeSystem),
         "--argstr", "bashRoot", str(Path(bash).parents[1]),
         "--argstr", "coreutilsRoot", str(Path(mkfifo).parents[1]),
         "--argstr", "workerRoot", str(worker.parents[1])],
        text=True,
    ).strip()
    output = subprocess.check_output(["nix-store", "--query", "--outputs", derivation], text=True).strip()
    closure = subprocess.check_output(["nix-store", "--query", "--requisites", derivation], text=True).splitlines()
    archive = peer.root / "card.archive"
    with archive.open("wb") as stream:
        subprocess.run(["nix-store", "--export", *closure], stdout=stream, check=True)
    with archive.open("rb") as stream:
        archive_digest = hashlib.file_digest(stream, "sha256").hexdigest()
        stream.seek(0)
        total = archive.stat().st_size
        for offset in range(0, total, 1 << 20):
            peer.put_chunk(job, archive_digest, total, offset, stream.read(1 << 20))

    schema = peer.root / "params.fbs"
    schema.write_text("table Params {} root_type Params;")
    params = peer.root / "params.json"
    params.write_text("{}")
    flatc = os.environ.get("TSUGUMI_FLATC", "flatc")
    subprocess.run([flatc, "--binary", "--schema", "-o", str(peer.root), str(schema)], check=True)
    subprocess.run([flatc, "--binary", "-o", str(peer.root), str(schema), str(params)], check=True)
    # The Card blocks on a FIFO, so only its call budget can produce a timeout.
    call = WorkerCallT(
        runId="1", card=CardPathT(segments=["deadline"]), invocation=1,
        inputs=[], params=FlatBufferDataT(
            schemaBfbs=schema.with_suffix(".bfbs").read_bytes(), rootType="Params",
            bytes=params.with_suffix(".bin").read_bytes(),
        ),
        outputs=[], workingDirectory="", temporaryDirectory="",
        environment=[EnvVarT(name="TSUGUMI_PARAMS_JSON", value="{}")],
        deadlineMs=budget_ms,
    )
    message = WorkerMessageT(callId=1, valueType=WorkerPayload.WorkerCall, value=call)
    call_digest = peer.upload(job, R.pack(message, b"TSW1"))
    invocation = peer.upload(job, json.dumps({
        "call_sha256": call_digest, "archive_sha256": archive_digest,
        "files": [], "directories": [], "network": False, "implementation_index": 1,
    }).encode())
    return R.pack(R.JobSpecT(
        system=peer.caps.nativeSystem, derivation=derivation, output=output,
        invocationSha256=invocation, inputBlobs=[archive_digest, call_digest, invocation],
        logLimitBytes=8 << 20,
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
    # deadline job uses the real namespace launcher and executes a Bash Card.
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
        submit(blocker, peer.spec(peer.upload(blocker, b"staged input")))
        peer.wait_state(blocker, R.JobState.Running)
        submit(next_job, peer.spec(peer.upload(next_job, b"next input")))
        assert state(next_job) == R.JobState.Queued
        peer.restart()
        assert state(blocker) == R.JobState.Running and state(next_job) == R.JobState.Queued
        cancel(blocker)
        peer.wait_state(blocker, R.JobState.Cancelled)
        peer.wait_state(next_job, R.JobState.Running)
        budget_ms = 1000
        submit(deadline_job, deadline_spec(peer, deadline_job, budget_ms))
        # A job held in the queue emits no event when its prospective Card
        # budget elapses. Keep the slot occupied for at least one such budget.
        time.sleep(budget_ms / 1000)
        assert state(deadline_job) == R.JobState.Queued
        cancel(next_job)
        # Succeeded means the supervisor imported a result. The Card outcome
        # is carried inside that result as a WorkerFailure.
        record = peer.wait_state(deadline_job, R.JobState.Succeeded)
        manifest_blob = peer.call(R.RequestBody.GetBlobRequest,
                                  R.GetBlobRequestT(digest=record["result"]), job=deadline_job)
        assert manifest_blob.bodyType == R.ReplyBody.BlobChunk
        manifest = R.ResultManifestT.InitFromPackedBuf(bytes(manifest_blob.body.bytes))
        result_blob = peer.call(R.RequestBody.GetBlobRequest,
                                R.GetBlobRequestT(digest=manifest.invocationResultSha256), job=deadline_job)
        assert result_blob.bodyType == R.ReplyBody.BlobChunk
        result = WorkerMessageT.InitFromPackedBuf(bytes(result_blob.body.bytes))
        assert result.valueType == WorkerPayload.WorkerFailure
        assert result.value.failure.kind == FailureKind.timeout, R.string(result.value.failure.diagnostic)
    finally:
        try:
            for job in fingerprints:
                cancel(job)
            for job in fingerprints:
                peer.wait_state(job, R.JobState.Cancelled, R.JobState.Succeeded,
                                R.JobState.Failed, R.JobState.Interrupted)
        finally:
            peer.close()


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
    launcher.write_text('#!/bin/sh\nfor arg do test "$arg" != --check-namespace || exit 0; done\n'
                        'echo partial-log\necho $$ > "$TEST_CHILD_PID"\nread -r line < "$TEST_GATE"\n')
    launcher.chmod(0o755)
    peer = Worker(tmp_path, binary, env={**os.environ,
        "PATH": str(tools) + os.pathsep + os.environ["PATH"],
        "TEST_CHILD_PID": str(child_pid), "TEST_GATE": str(gate)})
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
        peer.restart()
        peer.call(R.RequestBody.CancelRequest,
                  R.CancelRequestT(fingerprint=fingerprints[first]), job=first)
        peer.wait_state(first, R.JobState.Cancelled)
        wait_for_exit(pid)
        cancelled = peer.call(R.RequestBody.StatusRequest, R.StatusRequestT(), job=first)
        assert cancelled.body.state == R.JobState.Cancelled
        blob = peer.call(R.RequestBody.GetBlobRequest, R.GetBlobRequestT(digest=digest), job=first)
        assert blob.bodyType == R.ReplyBody.BlobChunk and bytes(blob.body.bytes) == b"orphan input"
        assert "partial-log" in (directory / "work" / "adapter.log").read_text()
        # A new life cannot collect the old result, but can use the freed slot.
        peer.core_life_id = "f" * 64
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
    for raw in (struct.pack("<I", (2 << 20) + 1), struct.pack("<I", 24) + bytes(24)):
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
        ready = worker.state / "worker.ready.json"

        def duplicate_ready():
            published = json.loads(ready.read_text())
            return published if published["pid"] == duplicate.pid else None

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
