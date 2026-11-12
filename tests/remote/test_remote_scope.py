"""The daemon's project boundary and durable requests."""

import unittest

from remote_fixtures import SCOPE_A, SCOPE_B, DaemonCase, sha

JOB = "c" * 64


class ScopeTests(DaemonCase):
    def test_instance_and_blob_access_are_isolated_by_project(self):
        a = self.scoped("probe", SCOPE_A)["instance_id"]
        b = self.scoped("probe", SCOPE_B)["instance_id"]
        self.assertNotEqual(a, b)
        self.assertEqual(self.scoped("probe", SCOPE_A)["instance_id"], a)

        payload = b"only project A may read this blob"
        digest = self.put(payload, SCOPE_A)
        reply, blob = self.call({"version": 1, "operation": "get_blob", "sha256": digest, "instance_id": a,
                                 "project_scope": SCOPE_A, "server_id": self.daemon.server_id})
        self.assertEqual((reply["ok"], blob), (True, payload))
        self.assertEqual(self.scoped("get_blob", SCOPE_B, instance_id=a, sha256=digest)["code"], "instance_changed")
        reply = self.scoped("get_blob", SCOPE_B, instance_id=b, sha256=digest)
        self.assertEqual((reply["ok"], "code" in reply), (False, False))

        spec = self.spec(digest)
        self.assertEqual(self.scoped("submit", SCOPE_A, spec, job_id=JOB, size=len(spec), instance_id=a)["state"],
                         "queued")
        self.assertEqual(self.scoped("submit", SCOPE_B, spec, job_id=JOB, size=len(spec), instance_id=b)["code"],
                         "request_rejected")
        self.assertEqual(self.scoped("status", SCOPE_B, job_id=JOB)["state"], "missing")

    def test_resubmission_keeps_the_original_snapshot(self):
        digest = self.put(b"original invocation")
        spec = self.spec(digest)
        self.assertEqual(self.submit(JOB, spec)["state"], "queued")
        self.assertEqual(self.submit(JOB, spec)["fingerprint"], sha(spec))
        self.assertEqual(self.submit(JOB, self.spec(digest, timeout=61))["code"], "request_rejected")
        self.assertEqual((self.root / "projects" / SCOPE_A / "jobs" / JOB / "spec").read_bytes(), spec)

    def test_cancel_before_submit_prevents_later_execution(self):
        spec = self.spec(self.put(b"cancelled invocation"))
        self.assertEqual(self.scoped("cancel", job_id=JOB, fingerprint=sha(spec))["state"], "cancelled")
        self.assertEqual(self.submit(JOB, spec)["state"], "cancelled")
        self.daemon.scheduler.tick()
        self.assertEqual(self.scoped("status", job_id=JOB)["state"], "cancelled")
        self.assertEqual(self.driver.spawned, [])


if __name__ == "__main__":
    unittest.main()
