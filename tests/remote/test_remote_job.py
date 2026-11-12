import hashlib
import io
import json
import os
import secrets
import struct
import tempfile
import threading
import unittest
from pathlib import Path

import remote_job
from remote_job import (
    ConflictError, Journal, NotFoundError, ValidationError,
    parse_result_manifest,
)

# Minimal FlatBuffers frame: root offset 8, vtable at 4 (len 4, table 4), soffset 4.
ENVELOPE = struct.pack("<IHHi", 8, 4, 4, 4)
DRV = "/nix/store/" + "a" * 32 + "-hello.drv"
OUT = "/nix/store/" + "b" * 32 + "-hello"


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class JournalTestCase(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name) / "journal"
        self.j = Journal(self.root)

    def put(self, data: bytes) -> str:
        digest = sha(data)
        self.j.put_blob(digest, len(data), io.BytesIO(data))
        return digest

    def spec_bytes(self, **overrides) -> bytes:
        inv = self.put(b"invocation")
        spec = {
            "system": "x86_64-linux", "derivation": DRV, "output": OUT,
            "invocation_sha256": inv, "input_blobs": [inv, self.put(b"source")],
            "cpu_millis": 1000, "memory_bytes": 1 << 30, "timeout_seconds": 600,
        }
        spec.update(overrides)
        return json.dumps(spec).encode()

    def submit_job(self, **overrides):
        job_id = secrets.token_hex(32)
        spec = self.spec_bytes(**overrides)
        state = self.j.submit(job_id, spec)
        return job_id, spec, state["fingerprint"]

    def running_job(self, claimant="pod-1"):
        job_id, spec, fp = self.submit_job()
        self.assertTrue(self.j.claim(job_id, claimant))
        return job_id, fp

    def manifest(self, files=None, envelope=ENVELOPE, sizes=None) -> str:
        files = files if files is not None else {"bin/hello": b"\x7fELF", "share/doc": b"doc"}
        outputs = []
        for path, data in files.items():
            size = (sizes or {}).get(path, len(data))
            outputs.append({"path": path, "sha256": self.put(data), "size": size})
        doc = {"outputs": outputs, "invocation_result_sha256": self.put(envelope)}
        return self.put(json.dumps(doc).encode())


class InstanceAndBlobTests(JournalTestCase):
    def test_instance_id_stable(self):
        self.assertRegex(self.j.instance_id, r"^[0-9a-f]{64}$")
        self.assertEqual(Journal(self.root).instance_id, self.j.instance_id)

    def test_blob_roundtrip_and_idempotent(self):
        digest = self.put(b"hello" * 100000)
        self.put(b"hello" * 100000)
        with self.j.open_blob(digest) as f:
            self.assertEqual(f.read(), b"hello" * 100000)
        with self.assertRaises(ConflictError):
            self.j.put_blob(digest, 3, io.BytesIO(b"abc"))

    def test_corrupt_blob_rejected_and_unpublished(self):
        digest = sha(b"good")
        with self.assertRaises(ValidationError):
            self.j.put_blob(digest, 4, io.BytesIO(b"evil"))
        with self.assertRaises(NotFoundError):
            self.j.open_blob(digest)
        self.assertEqual(os.listdir(self.root / "tmp"), [])

    def test_short_stream_rejected(self):
        digest = sha(b"abcdef")
        with self.assertRaises(ValidationError):
            self.j.put_blob(digest, 6, io.BytesIO(b"abc"))
        with self.assertRaises(NotFoundError):
            self.j.open_blob(digest)

    def test_only_declared_size_is_read(self):
        stream = io.BytesIO(b"abcdef")
        self.j.put_blob(sha(b"abc"), 3, stream)
        self.assertEqual(stream.read(), b"def")

    def test_bad_digest_and_size(self):
        for digest in ("../" + "a" * 61, "A" * 64, "a" * 63, None):
            with self.assertRaises(ValidationError):
                self.j.put_blob(digest, 0, io.BytesIO(b""))
        for size in (-1, True, remote_job.MAX_BLOB_BYTES + 1, 1.0):
            with self.assertRaises(ValidationError):
                self.j.put_blob(sha(b""), size, io.BytesIO(b""))


class SubmitTests(JournalTestCase):
    def test_duplicate_submit_and_mismatch(self):
        job_id, spec, fp = self.submit_job()
        self.assertEqual(fp, sha(spec))
        before = self.j.status(job_id)
        self.assertEqual(before["state"], "queued")
        self.assertEqual(self.j.submit(job_id, spec), before)
        with self.assertRaises(ConflictError):
            self.j.submit(job_id, self.spec_bytes(cpu_millis=2000))
        self.assertEqual(self.j.status(job_id), before)
        self.assertEqual(self.j.spec(job_id), spec)
        self.assertEqual(sorted(os.listdir(self.root / "jobs" / job_id)), ["spec", "state.json"])

    def test_public_state_shape(self):
        job_id, _, fp = self.submit_job()
        state = self.j.status(job_id)
        self.assertIsInstance(state.pop("deadline"), float)
        self.assertEqual(state, {
            "state": "queued", "fingerprint": fp, "claimant": None,
            "result_sha256": None, "error": None,
            "cancel_requested": False, "outcome_sha256": None,
        })

    def test_unknown_and_invalid_job_ids(self):
        self.assertIsNone(self.j.status("0" * 64))
        with self.assertRaises(NotFoundError):
            self.j.spec("0" * 64)
        for bad in ("../" + "0" * 61, "0" * 64 + "/x", "F" * 64):
            with self.assertRaises(ValidationError):
                self.j.status(bad)

    def test_resource_limits_are_an_optional_pair(self):
        spec = json.loads(self.spec_bytes())
        del spec["cpu_millis"], spec["memory_bytes"]
        self.assertEqual(self.j.submit(secrets.token_hex(32), json.dumps(spec).encode())["state"], "queued")
        for key in ("cpu_millis", "memory_bytes"):
            partial = json.loads(self.spec_bytes())
            del partial[key]
            job_id = secrets.token_hex(32)
            with self.subTest(missing=key), self.assertRaises(ValidationError):
                self.j.submit(job_id, json.dumps(partial).encode())
            self.assertIsNone(self.j.status(job_id))

    def test_job_ids(self):
        first, _, _ = self.submit_job()
        second, _, _ = self.submit_job()
        self.assertEqual(self.j.job_ids(), sorted([first, second]))

    def test_missing_input_blob_rejected(self):
        inv = self.put(b"invocation")
        spec = self.spec_bytes(input_blobs=[inv, sha(b"never published")])
        job_id = secrets.token_hex(32)
        with self.assertRaises(ValidationError):
            self.j.submit(job_id, spec)
        self.assertIsNone(self.j.status(job_id))

    def test_invalid_specs(self):
        inv = self.put(b"invocation")
        cases = [
            self.spec_bytes(cpu_millis=True),
            self.spec_bytes(cpu_millis=1.0),
            self.spec_bytes(cpu_millis=0),
            self.spec_bytes(memory_bytes=remote_job.MAX_MEMORY_BYTES + 1),
            self.spec_bytes(timeout_seconds=-5),
            self.spec_bytes(system="riscv64-linux"),
            self.spec_bytes(system=["x86_64-linux"]),
            self.spec_bytes(derivation=OUT),
            self.spec_bytes(output=OUT + "/bin"),
            self.spec_bytes(output="/nix/store/../etc"),
            self.spec_bytes(derivation="/nix/store/" + "e" * 32 + "-x.drv"),  # 'e' not nixbase32
            self.spec_bytes(input_blobs=[self.put(b"source")]),
            self.spec_bytes(input_blobs=[inv, inv]),
            self.spec_bytes(extra=1),
            json.dumps({"system": "x86_64-linux"}).encode(),
            b'{"system": "x86_64-linux", "system": "x86_64-linux"}',
            b"\xff\xfe",
            b"[]",
            b" " * (remote_job.MAX_SPEC_BYTES + 1),
        ]
        for spec in cases:
            job_id = secrets.token_hex(32)
            with self.subTest(spec=spec[:80]):
                with self.assertRaises(ValidationError):
                    self.j.submit(job_id, spec)
                self.assertIsNone(self.j.status(job_id))


class ClaimCancelTests(JournalTestCase):
    def test_claimed_then_reopened_no_second_claim(self):
        job_id, _, _ = self.submit_job()
        self.assertTrue(self.j.claim(job_id, "pod-1"))
        reopened = Journal(self.root)
        self.assertFalse(reopened.claim(job_id, "pod-1"))
        self.assertFalse(reopened.claim(job_id, "pod-2"))
        self.assertEqual(reopened.status(job_id)["claimant"], "pod-1")

    def test_claim_unknown_and_bad_claimant(self):
        self.assertFalse(self.j.claim("1" * 64, "pod-1"))
        job_id, _, _ = self.submit_job()
        for bad in ("../etc", "", "x" * 129, "a/b", None):
            with self.assertRaises(ValidationError):
                self.j.claim(job_id, bad)

    def test_concurrent_claim_threads(self):
        job_id, _, _ = self.submit_job()
        barrier = threading.Barrier(16)
        results = []

        def worker(i):
            journal = Journal(self.root)
            barrier.wait()
            results.append(journal.claim(job_id, f"pod-{i}"))

        threads = [threading.Thread(target=worker, args=(i,)) for i in range(16)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        self.assertEqual(results.count(True), 1)

    @unittest.skipUnless(hasattr(os, "fork"), "requires fork")
    def test_concurrent_claim_processes(self):
        job_id, _, _ = self.submit_job()
        pids = []
        for i in range(6):
            pid = os.fork()
            if pid == 0:
                code = 2
                try:
                    code = 0 if Journal(self.root).claim(job_id, f"pod-{i}") else 1
                finally:
                    os._exit(code)
            pids.append(pid)
        codes = [os.waitstatus_to_exitcode(os.waitpid(p, 0)[1]) for p in pids]
        self.assertEqual(sorted(codes), [0, 1, 1, 1, 1, 1])

    def test_cancel_before_submit(self):
        job_id = secrets.token_hex(32)
        spec = self.spec_bytes()
        tomb = self.j.cancel(job_id, sha(spec))
        self.assertEqual(tomb["state"], "cancelled")
        self.assertEqual(self.j.submit(job_id, spec), tomb)
        with self.assertRaises(ConflictError):
            self.j.submit(job_id, self.spec_bytes(cpu_millis=7))
        with self.assertRaises(ConflictError):
            self.j.cancel(job_id, sha(b"other"))
        with self.assertRaises(NotFoundError):
            self.j.spec(job_id)
        self.assertFalse(self.j.claim(job_id, "pod-1"))

    def test_cancel_queued(self):
        job_id, _, fp = self.submit_job()
        self.assertEqual(self.j.cancel(job_id, fp)["state"], "cancelled")
        self.assertFalse(self.j.claim(job_id, "pod-1"))
        self.assertEqual(self.j.cancel(job_id, fp)["state"], "cancelled")


class FinishTests(JournalTestCase):
    def test_success_idempotent_and_conflicts(self):
        job_id, fp = self.running_job()
        result = self.manifest()
        done = self.j.finish(job_id, "pod-1", "succeeded", result)
        self.assertEqual((done["state"], done["result_sha256"]), ("succeeded", result))
        self.assertRegex(done["outcome_sha256"], r"^[0-9a-f]{64}$")
        self.assertEqual(self.j.finish(job_id, "pod-1", "succeeded", result), done)
        with self.assertRaises(ConflictError):
            self.j.finish(job_id, "pod-1", "failed", error="boom")
        with self.assertRaises(ConflictError):
            self.j.finish(job_id, "pod-2", "succeeded", result)
        with self.assertRaises(ConflictError):
            self.j.mark_lost(job_id, "pod-1", "gone")
        # Completed first: cancel does not change it.
        self.assertEqual(self.j.cancel(job_id, fp), done)

    def test_finish_requires_claim_owner(self):
        job_id, _, _ = self.submit_job()
        with self.assertRaises(ConflictError):
            self.j.finish(job_id, "pod-1", "failed", error="x")
        self.assertTrue(self.j.claim(job_id, "pod-1"))
        with self.assertRaises(ConflictError):
            self.j.finish(job_id, "pod-2", "failed", error="x")
        with self.assertRaises(NotFoundError):
            self.j.finish("2" * 64, "pod-1", "failed")

    def test_finish_argument_validation(self):
        job_id, _ = self.running_job()
        result = self.manifest()
        bad_calls = [
            dict(state="cancelled"), dict(state="running"),
            dict(state="succeeded"), dict(state="succeeded", result_sha256=result, error="x"),
            dict(state="failed", result_sha256=result), dict(state="failed", error=""),
        ]
        for kwargs in bad_calls:
            with self.assertRaises(ValidationError):
                self.j.finish(job_id, "pod-1", **kwargs)
        self.assertEqual(self.j.status(job_id)["state"], "running")

    def test_failed_and_interrupted(self):
        job_id, _ = self.running_job()
        done = self.j.finish(job_id, "pod-1", "failed", error="exit 1")
        self.assertEqual((done["state"], done["error"], done["result_sha256"]),
                         ("failed", "exit 1", None))
        job_id, _ = self.running_job()
        self.assertEqual(self.j.finish(job_id, "pod-1", "interrupted")["state"], "interrupted")

    def test_cancel_running_then_finish_becomes_cancelled(self):
        job_id, fp = self.running_job()
        requested = self.j.cancel(job_id, fp)
        self.assertEqual((requested["state"], requested["cancel_requested"]), ("running", True))
        result = self.manifest()
        done = self.j.finish(job_id, "pod-1", "succeeded", result)
        self.assertEqual((done["state"], done["result_sha256"], done["error"]),
                         ("cancelled", None, None))
        self.assertNotIn(result, json.dumps(self.j.status(job_id)))
        self.assertNotIn(result.encode(), (self.root / "jobs" / job_id / "state.json").read_bytes())
        self.assertEqual(self.j.finish(job_id, "pod-1", "succeeded", result), done)
        with self.assertRaises(ConflictError):
            self.j.finish(job_id, "pod-1", "failed", error="x")

    def test_finish_racing_cancel(self):
        for _ in range(20):
            job_id, fp = self.running_job()
            result = self.manifest()
            barrier = threading.Barrier(2)

            def do_finish():
                barrier.wait()
                Journal(self.root).finish(job_id, "pod-1", "succeeded", result)

            def do_cancel():
                barrier.wait()
                Journal(self.root).cancel(job_id, fp)

            threads = [threading.Thread(target=do_finish), threading.Thread(target=do_cancel)]
            for t in threads:
                t.start()
            for t in threads:
                t.join()
            state = self.j.status(job_id)
            self.assertIn(state["state"], ("succeeded", "cancelled"))
            if state["state"] == "cancelled":
                self.assertIsNone(state["result_sha256"])
            else:
                self.assertEqual(state["result_sha256"], result)

    def test_success_requires_published_valid_result(self):
        job_id, _ = self.running_job()
        bad_results = [
            sha(b"never published"),
            self.put(b"not json"),
            self.manifest(sizes={"bin/hello": 999}),
            self.manifest(envelope=b"\x00" * 12),
            self.manifest(envelope=struct.pack("<IHHi", 8, 4, 64, 4)),
            self.manifest(envelope=b"tiny"),
            self.put(json.dumps({"outputs": [{"path": "a", "sha256": sha(b"absent"), "size": 6}],
                                 "invocation_result_sha256": self.put(ENVELOPE)}).encode()),
            self.put(json.dumps({"outputs": [{"path": "a", "sha256": self.put(b"a"), "size": 1}],
                                 "invocation_result_sha256": sha(b"absent")}).encode()),
        ]
        for result in bad_results:
            with self.assertRaises(ValidationError):
                self.j.finish(job_id, "pod-1", "succeeded", result)
            self.assertEqual(self.j.status(job_id)["state"], "running")

    def test_unsafe_result_paths(self):
        digest, env = sha(b"x"), sha(ENVELOPE)

        def doc(*paths):
            return json.dumps({"outputs": [{"path": p, "sha256": digest, "size": 1} for p in paths],
                               "invocation_result_sha256": env}).encode()

        parse_result_manifest(doc("bin/hello", "share/doc/a b", "lib/é"))
        unsafe = [
            ("",), ("/abs",), ("../x",), ("a/../b",), ("a//b",), ("a/",), ("./a",),
            ("a\\b",), ("a\nb",), ("a\x00b",), ("a\x7f",), ("a\x85",), ("x" * 256,),
            ("a/" * 2049,), ("a", "a"), ("a", "a/b"), ("a/b/c", "a/b"),
        ]
        for paths in unsafe:
            with self.subTest(paths=paths), self.assertRaises(ValidationError):
                parse_result_manifest(doc(*paths))
        for bad in [b'{"outputs": [], "invocation_result_sha256": "%s"}' % env.encode(),
                    json.dumps({"outputs": [{"path": "a", "sha256": digest, "size": True}],
                                "invocation_result_sha256": env}).encode(),
                    json.dumps({"outputs": [{"path": 1, "sha256": digest, "size": 1}],
                                "invocation_result_sha256": env}).encode(),
                    json.dumps({"outputs": [], "invocation_result_sha256": env, "x": 1}).encode()]:
            with self.assertRaises(ValidationError):
                parse_result_manifest(bad)
        # Through finish: unsafe manifest never commits.
        job_id, _ = self.running_job()
        result = self.manifest(files={"../escape": b"x"})
        with self.assertRaises(ValidationError):
            self.j.finish(job_id, "pod-1", "succeeded", result)
        self.assertEqual(self.j.status(job_id)["state"], "running")


class MarkLostTests(JournalTestCase):
    def test_mark_lost_interrupted_without_reclaim(self):
        job_id, _ = self.running_job()
        with self.assertRaises(ConflictError):
            self.j.mark_lost(job_id, "pod-2", "gone")
        lost = self.j.mark_lost(job_id, "pod-1", "pod disappeared")
        self.assertEqual((lost["state"], lost["error"]), ("interrupted", "pod disappeared"))
        self.assertEqual(self.j.mark_lost(job_id, "pod-1", "pod disappeared"), lost)
        self.assertFalse(self.j.claim(job_id, "pod-1"))
        self.assertFalse(Journal(self.root).claim(job_id, "pod-2"))
        with self.assertRaises(ConflictError):
            self.j.finish(job_id, "pod-1", "interrupted", error="pod disappeared")
        with self.assertRaises(ValidationError):
            self.j.mark_lost(job_id, "pod-1", None)

    def test_mark_lost_after_cancel_request(self):
        job_id, fp = self.running_job()
        self.j.cancel(job_id, fp)
        lost = self.j.mark_lost(job_id, "pod-1", "pod deleted")
        self.assertEqual((lost["state"], lost["result_sha256"]), ("cancelled", None))

    def test_no_temp_files_left(self):
        job_id, _ = self.running_job()
        self.j.finish(job_id, "pod-1", "succeeded", self.manifest())
        self.assertEqual(os.listdir(self.root / "tmp"), [])
        self.assertEqual(sorted(os.listdir(self.root / "jobs" / job_id)), ["spec", "state.json"])


if __name__ == "__main__":
    unittest.main()
