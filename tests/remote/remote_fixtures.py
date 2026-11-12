"""Shared helpers for the execution daemon tests (no listeners, no real supervisors)."""

import hashlib
import io
import json
import os
import struct
import tempfile
import unittest
from pathlib import Path

import remote_gateway as rg

NATIVE = "x86_64-linux"
STORE = "0123456789abcdfghijklmnpqrsvwxyz"
WORKER = "/opt/tsugumi/bin/tsugumi-remote-worker"
UNSHARE = "/usr/bin/unshare"
TOKEN = ("5e" * 32).encode()
SCOPE_A = "a" * 64
SCOPE_B = "b" * 64
FLATBUFFER = struct.pack("<IiHH", 4, -4, 4, 4)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def frame(header, body=b""):
    raw = json.dumps(header).encode()
    return struct.pack(">I", len(raw)) + raw + body


def decode(raw):
    (size,) = struct.unpack(">I", raw[:4])
    return json.loads(raw[4:4 + size]), raw[4 + size:]


class FakeDriver:
    """Stands in for detached supervisors: a live one holds a dup of the job lock."""

    def __init__(self):
        self.live, self.spawned, self.killed, self.fail = {}, [], [], None

    def spawn(self, argv, lock_fd, log_path):
        if self.fail:
            raise self.fail
        self.spawned.append(list(argv))
        # A dup shares the open file description, and so the flock, exactly
        # like the fd the real supervisor inherits.
        self.live[argv[4]] = os.dup(lock_fd)

    def reap(self):
        pass

    def kill(self, identity):
        self.killed.append(identity)

    def exit(self, job_id):
        os.close(self.live.pop(job_id))

    def jobs(self):
        return [argv[4] for argv in self.spawned]

    def close(self):
        for job_id in list(self.live):
            self.exit(job_id)


class FakeCgroups:
    root = Path("/sys/fs/cgroup/tsugumi-test")


def make_daemon(root, driver, **options):
    return rg.Daemon(root, token=TOKEN, system=NATIVE, worker=WORKER, unshare=UNSHARE,
                     driver=driver, **options)


class DaemonCase(unittest.TestCase):
    max_jobs = rg.DEFAULT_MAX_JOBS
    cgroups = None

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name) / "state"
        self.driver = FakeDriver()
        self.addCleanup(self.driver.close)
        self.daemon = make_daemon(self.root, self.driver, max_jobs=self.max_jobs, cgroups=self.cgroups)

    def call(self, header, body=b"", daemon=None):
        data = frame(header, body)
        raw, handle, size = rg.respond(daemon or self.daemon, rg.UploadBody(io.BytesIO(data), len(data)),
                                       header.get("server_id"))
        response, rest = decode(raw)
        self.assertEqual(rest, b"")
        blob = b""
        if handle is not None:
            with handle:
                blob = handle.read(size)
        return response, blob

    def scoped(self, operation, scope=SCOPE_A, body=b"", daemon=None, **fields):
        daemon = daemon or self.daemon
        header = {"version": 1, "operation": operation, "project_scope": scope,
                  "server_id": daemon.server_id, **fields}
        if operation != "probe" and "instance_id" not in header:
            header["instance_id"] = daemon.project(scope).journal.instance_id
        return self.call(header, body, daemon)[0]

    def put(self, data, scope=SCOPE_A):
        reply = self.scoped("put_blob", scope, data, sha256=sha(data), size=len(data))
        self.assertTrue(reply["ok"], reply)
        return sha(data)

    def spec(self, digest=None, *, limits=None, timeout=60, tag="card"):
        digest = digest or self.put(b"invocation-bytes")
        doc = {"system": NATIVE, "derivation": f"/nix/store/{STORE}-{tag}.drv",
               "output": f"/nix/store/{STORE}-{tag}", "invocation_sha256": digest,
               "input_blobs": [digest], "timeout_seconds": timeout}
        if limits:
            doc.update(cpu_millis=limits[0], memory_bytes=limits[1])
        return json.dumps(doc).encode()

    def submit(self, job_id, spec, scope=SCOPE_A, daemon=None):
        return self.scoped("submit", scope, spec, daemon, job_id=job_id, size=len(spec))

    def journal(self, scope=SCOPE_A, daemon=None):
        return (daemon or self.daemon).project(scope).journal
