import hashlib
import io
import json
import os
import re
import struct
import sys
import tempfile
import unittest
from pathlib import Path

import remote_gateway as rg
from remote_job import Journal, parse_result_manifest

NATIVE = "x86_64-linux"
STORE = "0123456789abcdfghijklmnpqrsvwxyz"
WORKER = "/opt/tsugumi/bin/tsugumi-remote-worker"
FLATBUFFER = struct.pack("<IiHH", 4, -4, 4, 4)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def frame(header, body=b""):
    raw = json.dumps(header).encode()
    return struct.pack(">I", len(raw)) + raw + body


class FakeSystemd:
    def __init__(self):
        self.units, self.calls, self.run_rc, self.show_rc = {}, [], 0, 0

    def __call__(self, argv):
        assert all(isinstance(arg, str) for arg in argv)
        self.calls.append(list(argv))
        if argv[0] == "systemd-run":
            unit = argv[2].split("=", 1)[1]
            if self.run_rc or unit in self.units:
                return self.run_rc or 1, "", "Failed to start\nunit exists"
            self.units[unit] = "active"
            return 0, "", ""
        unit = argv[-1]
        if argv[2] == "show":
            if self.show_rc:
                return self.show_rc, "", "Failed to connect to bus"
            state = self.units.get(unit)
            load = "not-found" if state is None else "loaded"
            return 0, f"LoadState={load}\nActiveState={state or 'inactive'}\nResult=exit-code\n", ""
        if argv[2] == "stop":
            return 0, "", ""
        raise AssertionError(argv)

    def ran(self, verb):
        return [c for c in self.calls if c[0] == verb or c[2:3] == [verb]]


class GatewayTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = os.path.join(self.tmp.name, "state")
        self.fake = FakeSystemd()
        self.gw = rg.Gateway(self.root, runner=self.fake, worker=WORKER, system=NATIVE)
        self.journal = self.gw.journal

    def tearDown(self):
        self.tmp.cleanup()

    def call(self, data):
        out = io.BytesIO()
        code = rg.serve(io.BytesIO(data), out, lambda: self.gw)
        raw = out.getvalue()
        (size,) = struct.unpack(">I", raw[:4])
        return code, json.loads(raw[4:4 + size]), raw[4 + size:]

    def request(self, body=b"", **header):
        return self.call(frame({"version": 1, **header}, body))

    def spec(self, cpu=1500):
        blob = b"invocation-bytes"
        self.assertTrue(self.request(blob, operation="put_blob", sha256=sha(blob),
                                     size=len(blob))[1]["ok"])
        return json.dumps({
            "system": NATIVE, "derivation": f"/nix/store/{STORE}-card.drv",
            "output": f"/nix/store/{STORE}-card", "invocation_sha256": sha(blob),
            "input_blobs": [sha(blob)], "cpu_millis": cpu, "memory_bytes": 1 << 30,
            "timeout_seconds": 60}).encode()

    def submit(self, job_id, spec, **extra):
        return self.request(spec, operation="submit", job_id=job_id, size=len(spec), **extra)[1]

    def test_malformed_frames_are_bounded(self):
        job = "a" * 64
        cases = [
            b"", b"\x00\x00", struct.pack(">I", 0), struct.pack(">I", 65537) + b"{}",
            struct.pack(">I", 10) + b"{}", struct.pack(">I", 3) + b"{x}",
            struct.pack(">I", 3) + b"[1]", struct.pack(">I", 2) + b"\xff\xfe",
            frame({"version": True, "operation": "probe"}),
            frame({"version": 2, "operation": "probe"}),
            frame({"version": 1, "operation": "exec"}),
            frame({"version": 1, "operation": "probe", "command": "rm -rf /"}),
            frame({"version": 1, "operation": "probe", "service": "a; reboot"}),
            frame({"version": 1, "operation": "status", "job_id": "../x"}),
            frame({"version": 1, "operation": "status", "job_id": job, "size": 1}),
            frame({"version": 1, "operation": "put_blob", "sha256": job}),
            frame({"version": 1, "operation": "submit", "job_id": job, "size": 1 << 20}),
            frame({"version": 1, "operation": "submit", "job_id": job, "size": 10}, b"{}"),
            struct.pack(">I", 25) + b'{"version":1,"version":1}',
        ]
        for data in cases:
            with self.subTest(data=data[:40]):
                code, resp, rest = self.call(data)
                self.assertEqual(code, 1)
                self.assertEqual((resp["version"], resp["ok"], rest), (1, False, b""))
                self.assertEqual(set(resp), {"version", "ok", "error"})
                self.assertTrue(0 < len(resp["error"]) <= rg.MAX_DIAGNOSTIC)
                self.assertNotIn("Traceback", resp["error"])
        self.assertEqual(self.fake.calls, [])

    def test_binary_round_trip(self):
        data = bytes(range(256)) * 5000  # spans several chunks
        code, resp, _ = self.request(data, operation="put_blob", sha256=sha(data), size=len(data))
        self.assertEqual((code, resp["ok"], resp["sha256"]), (0, True, sha(data)))
        code, resp, rest = self.request(operation="get_blob", sha256=sha(data))
        self.assertEqual((code, resp["ok"], resp["size"]), (0, True, len(data)))
        self.assertEqual(rest, data)
        bad = self.request(b"abc", operation="put_blob", sha256="0" * 64, size=3)[1]
        self.assertFalse(bad["ok"])
        missing = self.request(operation="get_blob", sha256="1" * 64)
        self.assertEqual((missing[0], missing[1]["ok"], missing[2]), (1, False, b""))

    def test_probe(self):
        code, resp, rest = self.request(operation="probe", service="tsugumi")
        self.assertEqual((code, rest), (0, b""))
        self.assertEqual(resp, {"version": 1, "ok": True, "instance_id": self.journal.instance_id,
                                "worker_contract": 1, "native_system": NATIVE, "systems": [NATIVE]})

    def test_submit_uses_opaque_unit_and_fixed_argv(self):
        job, spec = "b" * 64, self.spec(cpu=1500)
        resp = self.submit(job, spec, service="tsugumi-svc")
        self.assertEqual(resp, {"version": 1, "ok": True, "state": "queued",
                                "fingerprint": sha(spec)})
        (argv,) = self.fake.ran("systemd-run")
        unit = argv[2].split("=", 1)[1]
        self.assertRegex(unit, r"^tsugumi-card-[0-9a-f]{64}\.service$")
        self.assertNotIn(job, unit)
        self.assertEqual(unit, self.gw.unit(job))
        for prop in ("Type=exec", "CPUQuota=150%", f"MemoryMax={1 << 30}", "RuntimeMaxSec=60",
                     "KillMode=control-group", "MemoryAccounting=yes", "CPUAccounting=yes"):
            self.assertIn(f"--property={prop}", argv)
        self.assertEqual(argv[:2] + [argv[3]], ["systemd-run", "--user", "--no-block"])
        tail = argv[argv.index("--") + 1:]
        self.assertEqual(tail, [os.path.abspath(sys.executable), os.path.abspath(rg.__file__),
                                "--supervise", os.path.abspath(self.root), job, WORKER])
        self.assertTrue(all(os.path.isabs(p) for p in tail[:2] + [tail[3], tail[5]]))
        flat = " ".join(" ".join(c) for c in self.fake.calls)
        self.assertNotIn("tsugumi-svc", flat)
        self.assertFalse(re.search(r"[;&|`$]", flat))

    def test_duplicate_submit_and_conflict(self):
        job, spec = "c" * 64, self.spec()
        self.assertEqual(self.submit(job, spec)["state"], "queued")
        self.assertEqual(self.submit(job, spec)["state"], "queued")
        self.assertEqual(len(self.fake.ran("systemd-run")), 1)  # unit already active
        other = self.spec(cpu=2000)
        self.assertFalse(self.submit(job, other)["ok"])
        self.assertEqual(self.journal.status(job)["fingerprint"], sha(spec))

    def test_status_restarts_queued_job_with_missing_unit(self):
        job, spec = "d" * 64, self.spec()
        self.submit(job, spec)
        self.fake.units.clear()
        self.assertEqual(self.request(operation="status", job_id=job)[1]["state"], "queued")
        self.assertEqual(len(self.fake.ran("systemd-run")), 2)
        self.assertEqual(self.request(operation="status", job_id="e" * 64)[1],
                         {"version": 1, "ok": True, "state": "missing"})

    def test_launcher_failure_before_claim_is_diagnosed(self):
        job, spec = "d" * 64, self.spec()
        self.fake.run_rc = 1
        resp = self.submit(job, spec)
        self.assertEqual(resp["state"], "failed")
        self.assertIn("systemd-run failed", resp["diagnostic"])
        self.fake.run_rc = 0
        job2 = "f" * 64
        self.submit(job2, spec)
        self.fake.units[self.gw.unit(job2)] = "failed"
        resp = self.request(operation="status", job_id=job2)[1]
        self.assertEqual(resp["state"], "failed")
        self.assertIn("before claiming", resp["diagnostic"])

    def test_cancel_tombstone_precedes_submit(self):
        job, spec = "1" * 64, self.spec()
        resp = self.request(operation="cancel", job_id=job, fingerprint=sha(spec))[1]
        self.assertEqual(resp["state"], "cancelled")
        self.assertEqual(self.submit(job, spec)["state"], "cancelled")
        self.assertFalse(self.submit(job, self.spec(cpu=10))["ok"])
        self.assertEqual(self.fake.ran("systemd-run"), [])

    def test_cancel_queued_and_running(self):
        job, spec = "2" * 64, self.spec()
        self.submit(job, spec)
        self.assertEqual(self.request(operation="cancel", job_id=job,
                                      fingerprint=sha(spec))[1]["state"], "cancelled")
        self.assertEqual(self.fake.ran("stop"), [])
        self.assertEqual(rg.supervise(self.root, job, WORKER, adapter=self.fail, limit_checker=lambda _: None), 0)
        self.assertEqual(self.journal.status(job)["state"], "cancelled")

        job = "3" * 64
        self.submit(job, spec)
        self.assertTrue(self.journal.claim(job, "supervisor-test"))
        self.assertFalse(self.request(operation="cancel", job_id=job,
                                      fingerprint="0" * 64)[1]["ok"])
        resp = self.request(operation="cancel", job_id=job, fingerprint=sha(spec))[1]
        self.assertEqual(resp["state"], "cancelling")  # until termination is confirmed
        self.assertEqual(self.fake.ran("stop")[-1][-1], self.gw.unit(job))
        self.assertIn("--no-block", self.fake.ran("stop")[-1])
        del self.fake.units[self.gw.unit(job)]
        resp = self.request(operation="status", job_id=job)[1]
        self.assertEqual(resp["state"], "cancelled")
        self.assertNotIn("result_sha256", resp)

    def test_lost_claimed_job(self):
        job, spec = "4" * 64, self.spec()
        self.submit(job, spec)
        self.assertTrue(self.journal.claim(job, "supervisor-test"))
        self.assertEqual(self.request(operation="status", job_id=job)[1]["state"], "running")
        self.fake.units[self.gw.unit(job)] = "failed"
        resp = self.request(operation="status", job_id=job)[1]
        self.assertEqual(resp["state"], "interrupted")
        self.assertIn("without recording a result", resp["diagnostic"])

        job = "5" * 64
        self.submit(job, spec)
        self.journal.claim(job, "supervisor-test")
        self.fake.show_rc = 1  # query errors propagate, no invented outcome
        code, resp, _ = self.request(operation="status", job_id=job)
        self.assertEqual((code, resp["ok"]), (1, False))
        self.assertEqual(self.journal.status(job)["state"], "running")

    # ------------------------------------------------------- supervisor

    @staticmethod
    def fail(*_):
        raise AssertionError("adapter must not run")

    def test_supervisor_imports_results(self):
        job, spec = "6" * 64, self.spec()
        self.journal.submit(job, spec)
        seen = []

        def adapter(worker, job_dir, blob_dir):
            seen.append(((job_dir / "spec.json").read_bytes(), worker, blob_dir))
            self.assertEqual(os.stat(job_dir).st_mode & 0o077, 0)
            result = job_dir / "result"
            (result / "lib").mkdir(parents=True)
            (result / "worker.fb").write_bytes(FLATBUFFER)
            (result / "environment.json").write_bytes(b"{}")
            (result / "lib" / "a.bin").write_bytes(b"\x00\xff")

        self.assertEqual(rg.supervise(self.root, job, WORKER, adapter=adapter, limit_checker=lambda _: None), 0)
        self.assertEqual(seen, [(spec, WORKER, Path(self.root) / "blobs")])
        state = self.journal.status(job)
        self.assertEqual(state["state"], "succeeded")
        with self.journal.open_blob(state["result_sha256"]) as handle:
            manifest = parse_result_manifest(handle.read())
        self.assertEqual(manifest["invocation_result_sha256"], sha(FLATBUFFER))
        self.assertEqual(manifest["outputs"], [
            {"path": "environment.json", "sha256": sha(b"{}"), "size": 2},
            {"path": "lib/a.bin", "sha256": sha(b"\x00\xff"), "size": 2}])
        self.assertEqual(rg.supervise(self.root, job, WORKER, adapter=self.fail, limit_checker=lambda _: None), 0)

    def test_supervisor_rejects_symlinks_and_failures(self):
        spec = self.spec()

        def symlinking(worker, job_dir, blob_dir):
            result = job_dir / "result"
            result.mkdir()
            (result / "worker.fb").write_bytes(FLATBUFFER)
            os.symlink("/etc/passwd", result / "escape")

        def crashing(*_):
            raise OSError(5, "Input/output error")

        for job, adapter, needle in (("7" * 64, symlinking, "escape"),
                                     ("8" * 64, crashing, "Input/output error")):
            self.journal.submit(job, spec)
            self.assertEqual(rg.supervise(self.root, job, WORKER, adapter=adapter, limit_checker=lambda _: None), 0)
            state = self.journal.status(job)
            self.assertEqual(state["state"], "failed")
            self.assertIn(needle, state["error"])
            self.assertIsNone(state["result_sha256"])


if __name__ == "__main__":
    unittest.main()
