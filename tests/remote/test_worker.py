"""Black-box admission, scoping and recovery against the C++ remote worker.

The socket-free C++ suite covers journal races and protocol verification. This
suite runs on the Linux CI runner, outside the build sandbox.
"""
import concurrent.futures
import hashlib
import os
import ssl
import socket
import subprocess
import sys
import time

import pytest
from harness import remote_worker as R
from harness import peer_tls as TLS

pytestmark = pytest.mark.skipif(sys.platform != "linux", reason="remote workers require Linux")


class Worker:
    def __init__(self, root, binary, *, extra=(), env=None):
        self.root = root
        self.identity = TLS.identity(root)
        self.scope = "c" * 64
        with socket.socket() as temporary:
            temporary.bind(("127.0.0.1", 0))
            self.port = temporary.getsockname()[1]
        self.state = root / "state"
        self.state.mkdir(mode=0o700)
        self.authorized = self.state / "authorized_keys"
        self.authorized.write_text(TLS.public_key(self.identity) + " # test host\n")
        self.authorized.chmod(0o600)
        self.command = [binary, "--state-dir", str(self.state),
                        "--port", str(self.port), "--max-jobs", "1", *extra]
        self.env = env
        self.process = None
        self.start()
        self.caps = self.call(R.RequestBody.ProbeRequest, R.ProbeRequestT(), scoped=False).body
        self.instance = self.call(R.RequestBody.ProbeRequest, R.ProbeRequestT()).body.instanceId

    def start(self):
        self.log = (self.root / "worker.log").open("ab")
        self.process = subprocess.Popen(self.command, env=self.env, stdout=self.log, stderr=self.log)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError((self.root / "worker.log").read_text())
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=0.1):
                    return
            except OSError:
                time.sleep(0.02)
        raise TimeoutError("worker did not start")

    def close(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
            self.process.wait(timeout=20)
        self.log.close()

    def restart(self):
        self.process.kill()
        self.process.wait(timeout=5)
        self.log.close()
        self.start()

    def call(self, kind, body, *, job=None, scoped=True, unix=False, identity=None, **routing):
        fields = {}
        if scoped:
            fields.update(serverId=self.caps.serverId, projectScope=self.scope)
            if kind != R.RequestBody.ProbeRequest:
                fields["instanceId"] = self.instance
        fields.update(routing)
        if job:
            fields["jobId"] = job
        request = R.request(kind, body, **fields)
        peer = socket.socket(socket.AF_UNIX if unix else socket.AF_INET, socket.SOCK_STREAM)
        with peer:
            peer.settimeout(15)
            peer.connect(str(self.state / "worker.sock") if unix else ("127.0.0.1", self.port))
            context = TLS.client_context(identity or self.identity, self.state / "identity.pem")
            with context.wrap_socket(peer, server_hostname=None) as secure:
                secure.sendall(R.frame(request))
                with secure.makefile("rb") as stream:
                    return R.read(stream).payload

    def upload(self, job, data):
        digest = hashlib.sha256(data).hexdigest()
        response = self.call(R.RequestBody.PutBlobRequest, R.PutBlobRequestT(digest=digest, total=len(data), bytes=data), job=job)
        assert response.bodyType == R.ReplyBody.Ack
        return digest

    def spec(self, digest, timeout=30):
        return R.pack(R.JobSpecT(system=self.caps.nativeSystem,
            derivation="/nix/store/00000000000000000000000000000000-test.drv",
            output="/nix/store/00000000000000000000000000000000-test", invocationSha256=digest,
            inputBlobs=[digest], timeoutSeconds=timeout))


@pytest.fixture
def worker(tmp_path):
    binary = os.environ.get("TSUGUMI_REMOTE_TEST_DAEMON")
    if not binary:
        pytest.skip("set TSUGUMI_REMOTE_TEST_DAEMON to the packaged C++ worker")
    instance = Worker(tmp_path, binary)
    try:
        yield instance
    finally:
        instance.close()


def test_probe_authentication_unix_and_private_state(worker):
    assert worker.caps.workerContract == 3 and worker.caps.maxJobs == 1
    assert worker.caps.nativeSystem in (b"aarch64-linux", b"x86_64-linux")
    stranger = TLS.identity(worker.root, "stranger")
    with pytest.raises((ssl.SSLError, OSError, EOFError)):
        worker.call(R.RequestBody.ProbeRequest, R.ProbeRequestT(), scoped=False, identity=stranger)
    assert TLS.public_key(worker.state / "identity.pem").startswith("ed25519:")
    assert (worker.state / "identity.pem").stat().st_mode & 0o777 == 0o600
    local = worker.call(R.RequestBody.ProbeRequest, R.ProbeRequestT(), unix=True)
    assert local.body.instanceId == worker.instance
    before = (worker.caps.serverId, worker.instance)
    worker.restart()
    assert worker.call(R.RequestBody.ProbeRequest, R.ProbeRequestT()).body.instanceId == before[1]
    assert (worker.state.stat().st_mode & 0o777) == 0o700


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
    for _ in range(2):
        submitted = worker.call(R.RequestBody.SubmitRequest, R.SubmitRequestT(spec=spec), job=job)
        assert submitted.body.state == R.JobState.Cancelled
    for _ in range(2):
        released = worker.call(R.RequestBody.ReleaseRequest, R.ReleaseRequestT(fingerprint=fingerprint), job=job)
        assert released.bodyType == R.ReplyBody.Ack
    worker.restart()
    late = worker.call(R.RequestBody.SubmitRequest, R.SubmitRequestT(spec=spec), job=job)
    assert late.body.state == R.JobState.Released
    directory = worker.state / "projects" / worker.scope / "jobs" / job
    assert [p.name for p in directory.iterdir()] == ["state"]


def test_concurrent_submit_has_one_fingerprint(worker):
    job = "3" * 64
    digest = worker.upload(job, b"not an invocation")
    spec = worker.spec(digest)
    def submit(_):
        return worker.call(R.RequestBody.SubmitRequest, R.SubmitRequestT(spec=spec), job=job)
    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
        replies = list(pool.map(submit, range(12)))
    assert all(r.bodyType == R.ReplyBody.JobStatus and R.string(r.body.fingerprint) == hashlib.sha256(spec).hexdigest() for r in replies)
    mismatch = worker.call(R.RequestBody.SubmitRequest, R.SubmitRequestT(spec=worker.spec(digest, timeout=31)), job=job)
    assert mismatch.bodyType == R.ReplyBody.Failure


def test_rejects_unsafe_options_before_listening(tmp_path):
    binary = os.environ.get("TSUGUMI_REMOTE_TEST_DAEMON")
    if not binary:
        pytest.skip("needs packaged worker")
    for extra in (["--host", "not-an-IP"], ["--max-jobs", "0"], ["--state-dir", "relative"]):
        result = subprocess.run([binary, "--state-dir", str(tmp_path / "state"), *extra],
                                capture_output=True, timeout=10, check=False)
        assert result.returncode != 0
    state = tmp_path / "unsafe-state"
    state.mkdir(mode=0o700)
    pem = TLS.identity(state, "identity")
    pem.chmod(0o644)
    result = subprocess.run([binary, "--state-dir", str(state), "--public-key"], capture_output=True, timeout=10, check=False)
    assert result.returncode != 0


def test_global_cap_adopts_survivors_and_expires_queued_jobs(tmp_path):
    binary = os.environ.get("TSUGUMI_REMOTE_TEST_RAW_WORKER")
    if not binary:
        pytest.skip("needs the raw C++ executable for a controlled namespace launcher")
    tools = tmp_path / "tools"
    tools.mkdir()
    launcher = tools / "unshare"
    # The process double contains no Card code. exec preserves PDEATHSIG, so
    # killing the supervisor still tears it down without leaking a sleeper.
    launcher.write_text('#!/bin/sh\nfor arg do test "$arg" != --check-namespace || exit 0; done\nexec sleep 120\n')
    launcher.chmod(0o755)
    peer = Worker(tmp_path, binary, env={**os.environ, "PATH": str(tools) + os.pathsep + os.environ["PATH"]})
    jobs = [str(n) * 64 for n in (4, 5, 6)]
    fingerprints = {}
    def state(job):
        return peer.call(R.RequestBody.StatusRequest, R.StatusRequestT(), job=job).body.state
    def wait(job, expected):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if state(job) == expected:
                return
            time.sleep(0.05)
        raise AssertionError(f"job did not reach {expected}")
    try:
        for i, job in enumerate(jobs):
            digest = peer.upload(job, b"staged input")
            spec = peer.spec(digest, timeout=1 if i == 2 else 60)
            fingerprints[job] = hashlib.sha256(spec).hexdigest()
            accepted = peer.call(R.RequestBody.SubmitRequest, R.SubmitRequestT(spec=spec), job=job)
            assert accepted.bodyType == R.ReplyBody.JobStatus
            if i == 0:
                wait(job, R.JobState.Running)
        assert state(jobs[1]) == R.JobState.Queued
        peer.restart()
        assert state(jobs[0]) == R.JobState.Running and state(jobs[1]) == R.JobState.Queued
        wait(jobs[2], R.JobState.Failed)
        peer.call(R.RequestBody.CancelRequest, R.CancelRequestT(fingerprint=fingerprints[jobs[0]]), job=jobs[0])
        wait(jobs[0], R.JobState.Cancelled)
        wait(jobs[1], R.JobState.Running)
    finally:
        for job, fingerprint in fingerprints.items():
            peer.call(R.RequestBody.CancelRequest, R.CancelRequestT(fingerprint=fingerprint), job=job)
        for job in jobs[:2]:
            if job in fingerprints:
                wait(job, R.JobState.Cancelled)
        peer.close()


def test_bounded_frames_and_single_listener(worker):
    import struct
    for raw in (struct.pack("<I", (2 << 20) + 1), struct.pack("<I", 24) + bytes(24)):
        with socket.create_connection(("127.0.0.1", worker.port), timeout=5) as connection:
            context = TLS.client_context(worker.identity, worker.state / "identity.pem")
            with context.wrap_socket(connection, server_hostname=None) as secure:
                secure.sendall(raw)
                with secure.makefile("rb") as stream:
                    error = R.read(stream).payload
            assert error.bodyType == R.ReplyBody.Failure and error.body.code == R.ErrorCode.Invalid
    duplicate = subprocess.run(worker.command, capture_output=True, timeout=10, check=False)
    assert duplicate.returncode != 0 and b"another remote worker" in duplicate.stderr


def test_limits_require_delegation(worker):
    job = "9" * 64
    digest = worker.upload(job, b"input")
    spec = R.JobSpecT.InitFromPackedBuf(worker.spec(digest))
    spec.cpuMillis = 500
    spec.memoryBytes = 256 << 20
    response = worker.call(R.RequestBody.SubmitRequest, R.SubmitRequestT(spec=R.pack(spec)), job=job)
    assert response.bodyType == R.ReplyBody.Failure and response.body.code == R.ErrorCode.Rejected


def test_authorized_key_removal_refuses_new_connections(worker):
    worker.authorized.write_text("# No authorized hosts\n")
    with pytest.raises((ssl.SSLError, OSError, EOFError)):
        worker.call(R.RequestBody.ProbeRequest, R.ProbeRequestT(), scoped=False)
