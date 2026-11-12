"""Exercise the production gateway's project boundary and durable requests."""

import hashlib
import io
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

import remote_gateway


SCOPE_A = "a" * 64
SCOPE_B = "b" * 64
JOB = "c" * 64
NATIVE = "x86_64-linux"


class DroppedReply(io.BytesIO):
    def write(self, data):
        raise BrokenPipeError("caller disconnected before receiving the receipt")


class ScopeTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name) / "state"
        self.enterContext(patch.dict(os.environ, {"TSUGUMI_REMOTE_STATE_DIR": str(self.root)}))
        self.enterContext(patch.object(remote_gateway, "native_system", return_value=NATIVE))
        self.query = self.enterContext(patch.object(remote_gateway.Gateway, "_query", return_value={
            "LoadState": "loaded", "ActiveState": "activating",
        }))

    def request(self, operation, *, scope=None, body=b"", output=None, **fields):
        header = {"version": 1, "operation": operation, **fields}
        if scope is not None:
            header["project_scope"] = scope
        data = json.dumps(header).encode()
        response = io.BytesIO() if output is None else output
        code = remote_gateway.serve(io.BytesIO(struct.pack(">I", len(data)) + data + body), response)
        if output is not None:
            return code
        raw = response.getvalue()
        size, = struct.unpack(">I", raw[:4])
        return code, json.loads(raw[4:4 + size]), raw[4 + size:]

    def put(self, scope, body):
        digest = hashlib.sha256(body).hexdigest()
        code, reply, _ = self.request("put_blob", scope=scope, body=body, sha256=digest, size=len(body))
        self.assertEqual((code, reply["sha256"]), (0, digest))
        return digest

    def spec(self, digest, cpu=1000):
        return json.dumps({
            "system": NATIVE, "derivation": "/nix/store/" + "0" * 32 + "-card.drv",
            "output": "/nix/store/" + "0" * 32 + "-card", "invocation_sha256": digest,
            "input_blobs": [digest], "cpu_millis": cpu, "memory_bytes": 64 << 20,
            "timeout_seconds": 30,
        }).encode()

    def submit(self, scope, spec, **fields):
        return self.request("submit", scope=scope, job_id=JOB, size=len(spec), body=spec, **fields)

    def test_unscoped_probe_only_exposes_capabilities(self):
        code, probe, body = self.request("probe")
        self.assertEqual((code, probe["native_system"], body), (0, NATIVE, b""))
        self.assertNotIn("instance_id", probe)
        for scope in (None, "../" + SCOPE_A):
            code, reply, body = self.request("status", scope=scope, job_id=JOB)
            self.assertEqual((code, reply["ok"], body), (1, False, b""))
        self.assertFalse(self.root.exists())

    def test_instance_and_blob_access_are_isolated_by_project(self):
        _, project_a, _ = self.request("probe", scope=SCOPE_A)
        _, project_b, _ = self.request("probe", scope=SCOPE_B)
        a, b = project_a["instance_id"], project_b["instance_id"]
        self.assertNotEqual(a, b)
        self.assertEqual(self.request("probe", scope=SCOPE_A)[1]["instance_id"], a)

        payload = b"only project A may read this blob"
        digest = self.put(SCOPE_A, payload)
        self.assertEqual(self.request("get_blob", scope=SCOPE_A, instance_id=a, sha256=digest)[2], payload)
        code, reply, body = self.request("get_blob", scope=SCOPE_B, instance_id=a, sha256=digest)
        self.assertEqual((code, reply["code"], body), (1, "instance_changed", b""))
        code, reply, body = self.request("get_blob", scope=SCOPE_B, instance_id=b, sha256=digest)
        self.assertEqual((code, reply["ok"], body), (1, False, b""))

        spec = self.spec(digest)
        self.assertEqual(self.submit(SCOPE_A, spec, instance_id=a)[1]["state"], "queued")
        self.assertEqual(self.submit(SCOPE_B, spec, instance_id=b)[1]["code"], "request_rejected")
        self.assertEqual(self.request("status", scope=SCOPE_B, instance_id=b, job_id=JOB)[1]["state"], "missing")

    def test_receipt_loss_keeps_the_original_submission_snapshot(self):
        digest = self.put(SCOPE_A, b"original invocation")
        spec = self.spec(digest)
        self.assertEqual(self.submit(SCOPE_A, spec, output=DroppedReply()), 1)

        fingerprint = hashlib.sha256(spec).hexdigest()
        self.assertEqual(self.request("status", scope=SCOPE_A, job_id=JOB)[1]["fingerprint"], fingerprint)
        self.assertEqual(self.submit(SCOPE_A, spec)[1]["fingerprint"], fingerprint)
        self.assertEqual(self.submit(SCOPE_A, self.spec(digest, cpu=2000))[1]["code"], "request_rejected")
        self.assertEqual((self.root / "projects" / SCOPE_A / "jobs" / JOB / "spec").read_bytes(), spec)

    def test_cancel_before_submit_prevents_later_execution(self):
        spec = self.spec(self.put(SCOPE_A, b"cancelled invocation"))
        fingerprint = hashlib.sha256(spec).hexdigest()
        self.assertEqual(self.request("cancel", scope=SCOPE_A, job_id=JOB, fingerprint=fingerprint)[1]["state"],
                         "cancelled")
        self.assertEqual(self.submit(SCOPE_A, spec)[1]["state"], "cancelled")
        self.assertEqual(self.request("status", scope=SCOPE_A, job_id=JOB)[1]["state"], "cancelled")
        self.query.assert_not_called()
