"""Black-box admission, scoping and core lifetimes against tsugumi-workerd.

The socket-free C++ suite covers journal races and protocol verification. This
suite runs via tests/ci-remote-worker.sh on Linux, outside the build sandbox.
"""
import concurrent.futures
import fcntl
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
from Tsugumi.RemoteWorker.WorkerReady import WorkerReady
from Tsugumi.RemoteWorker.DisplayCredit import DisplayCreditT
from harness import peer_tls as TLS
from harness.file_events import process_file, wait_for_exit
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.LogStream import LogStream
from tsugumi.ArtifactId import ArtifactIdT
from Tsugumi.Card.CardPort import CardPortT
from Tsugumi.Worker.ArtifactGrant import ArtifactGrantT
from Tsugumi.Worker.WorkerPort import WorkerPortT
from test_worker_loading import card_package
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


def card_spec(peer, job, body, *, deadline_ms=None, outputs=(), inputs=(), files=(),
              grants=(), output_budget=None):
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
        inputs=list(inputs), outputs=list(outputs), grants=list(grants),
        deadlineMs=deadline_ms,
    )
    message = WorkerMessageT(callId=1, valueType=WorkerPayload.WorkerCall, value=call)
    call_digest = peer.upload(job, R.pack(message, b"TSW1"))
    invocation = peer.upload(job, R.pack(R.InvocationT(
        callSha256=call_digest, archiveSha256=archive_digest,
        files=[R.InvocationFileT(**file) for file in files], implementationIndex=1,
    )))
    return R.pack(R.JobSpecT(
        system=peer.caps.nativeSystem, derivation=derivation, output=output,
        invocationSha256=invocation, inputBlobs=[archive_digest, call_digest, invocation, *[file["sha256"] for file in files]],
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
                                R.JobState.Failed, R.JobState.Interrupted)
        finally:
            peer.close()


@pytest.mark.parametrize("case", ["artifact", "symlink", "byte-budget", "entry-budget"])
def test_real_remote_artifact_boundary(worker, case):
    job = "9" * 64
    outputs = [WorkerPortT(name="doc", required=True)]
    inputs = []
    grants = []
    files = []
    create = """
      tsugumi_artifact_id id;
      const auto directory = fs::path(create_artifact(call, id));
    """
    if case == "artifact":
        granted = ArtifactIdT(bytes=[3] * 16)
        relative = f"1/group/source/@2/{bytes(granted.bytes).hex()}"
        contents = b"staged"
        files.append({"path": f"artifacts/{relative}/nested/input.txt",
                      "sha256": worker.upload(job, contents), "size": len(contents)})
        inputs.append(CardPortT(name="doc", artifacts=[granted]))
        grants.append(ArtifactGrantT(id=granted, path=relative))
        body = create + """
          const fs::path root = "/tsugumi-artifacts";
          const fs::path granted = open_artifact(call, in);
          if (granted.lexically_relative(root).generic_string() !=
              "1/group/source/@2/03030303030303030303030303030303") return 2;
          for (auto holder = granted.parent_path(); ; holder = holder.parent_path()) {
            std::error_code error;
            fs::directory_iterator listing(holder, error);
            if (error != std::errc::permission_denied ||
                chmod(holder.c_str(), 0700) == 0) return 3;
            if (holder == root) break;
          }
          std::string contents;
          const auto input = granted / "nested/input.txt";
          std::ifstream(input) >> contents;
          if (contents != "staged" ||
              std::ofstream(input, std::ios::app) ||
              chmod(input.c_str(), 0600) == 0) return 4;
          static const uint8_t first[] = {0, 0};
          static const uint8_t second[] = {0, 255, 128};
          if (call.effects->send(call.host, "data", first, sizeof(first)) ||
              call.effects->send(call.host, "data", second, sizeof(second))) return 1;
          std::ofstream(directory / "report.txt") << "artifact";
          std::ofstream("unrelated") << "not an artifact";
          out.artifact(id);
        """
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
    spec = card_spec(worker, job, body, outputs=outputs, inputs=inputs,
                     grants=grants, files=files)
    fingerprint = hashlib.sha256(spec).hexdigest()
    try:
        response = worker.call(R.RequestBody.SubmitRequest, R.SubmitRequestT(spec=spec), job=job)
        assert response.bodyType == R.ReplyBody.JobStatus, vars(response.body)
        displays = []
        display_after = 0
        released = {}
        deadline = time.monotonic() + 300
        while True:
            # Advance past replayed displays and return credit for consumed bytes.
            query = R.StatusRequestT(displayAfter=display_after, displayCredits=[
                DisplayCreditT(view=view, released=total)
                for view, total in released.items()
            ])
            response = worker.call(R.RequestBody.StatusRequest, query, job=job)
            assert response.bodyType == R.ReplyBody.JobStatus, vars(response.body)
            status = response.body
            if status.displaySequence > display_after:
                display_after = status.displaySequence
                for message in status.displays:
                    view = R.string(message.view)
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
            assert displays == [("data", b"\x00\x00"), ("data", b"\x00\xff\x80")]
            created, = result.value.created
            output, = result.value.outputs
            assert bytes(output.artifacts[0].bytes) == bytes(created.bytes)
            artifact_id = bytes(created.bytes).hex()
            artifact_path = f"files/artifacts/{artifact_id}/report.txt"
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

    rows = listed()
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
