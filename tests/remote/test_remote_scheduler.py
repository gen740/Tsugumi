"""The daemon-owned queue, restart adoption, cancellation and the job supervisor."""

import fcntl
import json
import os
import sys
import threading
import time
import unittest
from pathlib import Path
from unittest.mock import patch

import remote_gateway as rg
from remote_job import parse_result_manifest
from remote_fixtures import (
    FLATBUFFER, SCOPE_A, SCOPE_B, UNSHARE, WORKER, DaemonCase, FakeDriver, make_daemon, sha,
)

SLEEPER = [sys.executable, "-c", "import time; time.sleep(60)"]


class SchedulerTests(DaemonCase):
    max_jobs = 2

    def queue(self, *jobs, scope=SCOPE_A):
        spec = self.spec(self.put(b"invocation", scope))
        for job in jobs:
            self.assertEqual(self.submit(job, spec, scope)["state"], "queued")
        return spec

    def test_global_cap_fifo_and_independence_from_polling(self):
        first, second, third = "1" * 64, "2" * 64, "3" * 64
        self.queue(first, second)
        self.queue(third, scope=SCOPE_B)  # the cap is global across projects
        for _ in range(3):
            self.scoped("status", job_id=first)
        self.assertEqual(self.driver.spawned, [])  # neither submit nor status launches

        self.daemon.scheduler.tick()
        self.daemon.scheduler.tick()
        self.assertEqual(self.driver.jobs(), [first, second])
        project = self.daemon.project(SCOPE_A)
        argv = self.driver.spawned[0]
        self.assertEqual(argv[2:7], ["--supervise", str(project.root), first, WORKER, UNSHARE])
        self.assertRegex(argv[7], r"^[0-9]+$")
        self.assertEqual(argv[8], "-")

        journal = self.journal()
        self.assertTrue(journal.claim(first, "supervisor-test"))
        journal.finish(first, "supervisor-test", "failed", error="done")
        self.driver.exit(first)
        self.daemon.scheduler.tick()
        self.assertEqual(self.driver.jobs(), [first, second, third])
        self.assertEqual(self.daemon.scheduler.active, 2)
        self.assertFalse(project.lock_path(first).exists())

    def test_restart_adopts_survivors_and_restores_only_pending_jobs(self):
        running, starting, pending, lost = "4" * 64, "5" * 64, "6" * 64, "7" * 64
        spec = self.queue(running, starting, pending)
        self.daemon.scheduler.tick()
        journal = self.journal()
        self.assertTrue(journal.claim(running, "supervisor-a"))
        # `starting` has a supervisor that has not claimed yet (or the daemon
        # died mid-spawn). `lost` was claimed by a supervisor that is gone.
        journal.submit(lost, spec)
        self.assertTrue(journal.claim(lost, "supervisor-gone"))

        # The daemon dies; supervisors (self.driver) survive.
        driver = FakeDriver()
        self.addCleanup(driver.close)
        daemon = make_daemon(self.root, driver, max_jobs=2)
        daemon.scheduler.recover()
        daemon.scheduler.tick()
        self.assertEqual(driver.spawned, [])
        journal = self.journal(daemon=daemon)
        self.assertEqual(journal.status(lost)["state"], "interrupted")
        self.assertIn("without recording a result", journal.status(lost)["error"])
        self.assertEqual(journal.status(starting)["state"], "queued")
        self.assertEqual(journal.status(running)["claimant"], "supervisor-a")

        self.driver.exit(starting)  # ended before claiming: safe to launch again
        daemon.scheduler.tick()
        self.assertEqual(driver.jobs(), [starting])
        journal.finish(running, "supervisor-a", "failed", error="done")
        self.driver.exit(running)
        daemon.scheduler.tick()
        self.assertEqual(driver.jobs(), [starting, pending])

    def test_queued_deadline_survives_restart_and_prevents_worker_launch(self):
        job, delayed_claim = "0" * 64, "a" * 64
        spec = self.spec(timeout=1)
        with patch("remote_job.time.time", return_value=100.0):
            self.assertEqual(self.submit(job, spec)["state"], "queued")
            self.assertEqual(self.submit(delayed_claim, spec)["state"], "queued")
        driver = FakeDriver()
        self.addCleanup(driver.close)
        daemon = make_daemon(self.root, driver)
        daemon.scheduler.recover()
        with patch("remote_job.time.time", return_value=102.0):
            journal = self.journal(daemon=daemon)
            self.assertFalse(journal.claim(delayed_claim, "too-late"))
            daemon.scheduler.tick()
            self.assertEqual(self.submit(job, spec, daemon=daemon)["state"], "failed")
        self.assertEqual(driver.spawned, [])
        state = self.journal(daemon=daemon).status(job)
        self.assertEqual(state["deadline"], 101.0)
        self.assertIn("timed out after waiting", state["error"])

    def test_supervisor_that_never_claims_fails_the_job(self):
        job = "8" * 64
        self.queue(job)
        for attempt in range(rg.MAX_LAUNCH_ATTEMPTS):
            self.daemon.scheduler.tick()
            self.assertEqual(len(self.driver.spawned), attempt + 1)
            self.driver.exit(job)
        self.daemon.scheduler.tick()
        state = self.scoped("status", job_id=job)
        self.assertEqual(state["state"], "failed")
        self.assertIn("before claiming", state["diagnostic"])

    def test_launch_failure_is_diagnosed(self):
        job = "9" * 64
        self.queue(job)
        self.driver.fail = OSError(8, "Exec format error")
        self.daemon.scheduler.tick()
        state = self.scoped("status", job_id=job)
        self.assertEqual(state["state"], "failed")
        self.assertIn("Exec format error", state["diagnostic"])

    def test_cancel_queued_never_launches(self):
        job = "d" * 64
        spec = self.queue(job)
        self.assertEqual(self.scoped("cancel", job_id=job, fingerprint=sha(spec))["state"], "cancelled")
        self.daemon.scheduler.tick()
        self.assertEqual(self.driver.spawned, [])

    def running(self, job, deadline):
        spec = self.queue(job)
        self.daemon.scheduler.tick()
        self.assertTrue(self.journal().claim(job, "supervisor-test"))
        identity = {"pid": 4242, "start": 1, "boot_id": "boot", "deadline": deadline}
        self.daemon.project(SCOPE_A).identity_path(job).write_text(json.dumps(identity))
        return spec, identity

    def test_unresponsive_cancellation_kills_the_supervisor_after_grace(self):
        job = "e" * 64
        spec, identity = self.running(job, time.time() + 3600)
        self.assertEqual(self.scoped("cancel", job_id=job, fingerprint=sha(spec))["state"], "cancelling")
        self.daemon.scheduler.tick()
        self.assertEqual(self.driver.killed, [])
        with patch.object(rg, "KILL_GRACE_SECONDS", 0):
            self.daemon.scheduler.tick()
        self.assertEqual(self.driver.killed, [identity])
        self.driver.exit(job)  # the killed supervisor releases its lock
        self.daemon.scheduler.tick()
        state = self.scoped("status", job_id=job)
        self.assertEqual(state["state"], "cancelled")
        self.assertNotIn("result_sha256", state)
        self.assertFalse(self.daemon.project(SCOPE_A).identity_path(job).exists())

    def test_overdue_supervisor_is_killed(self):
        job = "f" * 64
        _, identity = self.running(job, time.time() - rg.KILL_GRACE_SECONDS - 1)
        self.daemon.scheduler.tick()
        self.assertEqual(self.driver.killed, [identity])


class SupervisorTests(DaemonCase):
    def setUp(self):
        super().setUp()
        self.project = self.daemon.project(SCOPE_A)

    def queued(self, job, **options):
        spec = self.spec(**options)
        self.journal().submit(job, spec)  # as accepted earlier, possibly by another configuration
        return spec

    def supervise(self, job, sandbox, cgroup_root="-"):
        fd = rg._open_lock(self.project.lock_path(job))
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        try:
            return rg.supervise(str(self.project.root), job, WORKER, UNSHARE, fd, cgroup_root, sandbox=sandbox)
        finally:
            os.close(fd)

    @staticmethod
    def fail(*_):
        raise AssertionError("sandbox must not run")

    @staticmethod
    def sleeper(argv, job_dir, should_stop, cgroup):
        rg.run_sandbox(SLEEPER, job_dir, should_stop, cgroup)

    def test_runs_the_namespace_launcher_and_imports_results(self):
        job = "6" * 64
        spec = self.queued(job)
        seen = []

        def sandbox(argv, job_dir, should_stop, cgroup):
            seen.append((argv, (job_dir / "spec.json").read_bytes(), cgroup, should_stop()))
            self.assertEqual(os.stat(job_dir).st_mode & 0o077, 0)
            result = job_dir / "result"
            (result / "lib").mkdir(parents=True)
            (result / "worker.fb").write_bytes(FLATBUFFER)
            (result / "environment.json").write_bytes(b"{}")
            (result / "lib" / "a.bin").write_bytes(b"\x00\xff")

        self.assertEqual(self.supervise(job, sandbox), 0)
        work = self.project.root / "work" / job
        self.assertEqual(seen, [([UNSHARE, "--map-current-user", "--pid", "--fork", "--kill-child=SIGKILL",
                                  "--mount-proc", "--", WORKER, str(work), str(self.project.root / "blobs")],
                                 spec, None, None)])
        self.assertFalse(work.exists())
        identity = json.loads(self.project.identity_path(job).read_bytes())
        self.assertEqual(identity["pid"], os.getpid())
        state = self.journal().status(job)
        self.assertEqual(state["state"], "succeeded")
        with self.journal().open_blob(state["result_sha256"]) as handle:
            manifest = parse_result_manifest(handle.read())
        self.assertEqual(manifest["invocation_result_sha256"], sha(FLATBUFFER))
        self.assertEqual(manifest["outputs"], [
            {"path": "environment.json", "sha256": sha(b"{}"), "size": 2},
            {"path": "lib/a.bin", "sha256": sha(b"\x00\xff"), "size": 2}])
        self.assertEqual(self.supervise(job, self.fail), 0)  # claimed at most once

    def test_requires_the_inherited_job_lock(self):
        job = "7" * 64
        self.queued(job)
        path = self.project.lock_path(job)
        holder = rg._open_lock(path)
        fcntl.flock(holder, fcntl.LOCK_EX)
        self.addCleanup(os.close, holder)
        other = rg._open_lock(path)
        self.addCleanup(os.close, other)
        unrelated = os.open(self.project.root / "instance_id", os.O_RDONLY)
        self.addCleanup(os.close, unrelated)
        root = str(self.project.root)
        for fd in (other, unrelated):
            self.assertEqual(rg.supervise(root, job, WORKER, UNSHARE, fd, "-", sandbox=self.fail), 2)
        self.assertEqual(rg.supervise("relative", job, WORKER, UNSHARE, holder, "-", sandbox=self.fail), 2)
        self.assertEqual(self.journal().status(job)["state"], "queued")

    def test_rejects_symlinks_and_failures(self):
        def symlinking(argv, job_dir, should_stop, cgroup):
            result = job_dir / "result"
            result.mkdir()
            (result / "worker.fb").write_bytes(FLATBUFFER)
            os.symlink("/etc/passwd", result / "escape")

        def crashing(*_):
            raise OSError(5, "Input/output error")

        def exiting(argv, job_dir, should_stop, cgroup):
            rg.run_sandbox([sys.executable, "-c", "print('boom'); raise SystemExit(3)"], job_dir, should_stop, cgroup)

        for job, sandbox, needles in (("1" * 64, symlinking, ["escape"]),
                                      ("2" * 64, crashing, ["Input/output error"]),
                                      ("3" * 64, exiting, ["status 3", "boom"])):
            self.queued(job)
            self.assertEqual(self.supervise(job, sandbox), 0)
            state = self.journal().status(job)
            self.assertEqual(state["state"], "failed")
            for needle in needles:
                self.assertIn(needle, state["error"])
            self.assertIsNone(state["result_sha256"])

    def test_limits_without_delegated_cgroups_fail_clearly(self):
        job = "4" * 64
        self.queued(job, limits=(1000, 1 << 30))
        self.assertEqual(self.supervise(job, self.fail), 0)
        state = self.journal().status(job)
        self.assertEqual(state["state"], "failed")
        self.assertIn("--cgroup-root", state["error"])

    def test_cancellation_kills_the_sandbox(self):
        job = "5" * 64
        spec = self.queued(job)
        started = threading.Event()

        def sandbox(*args):
            started.set()
            self.sleeper(*args)

        def cancel():
            started.wait(10)
            self.journal().cancel(job, sha(spec))

        thread = threading.Thread(target=cancel)
        thread.start()
        begin = time.monotonic()
        self.assertEqual(self.supervise(job, sandbox), 0)
        thread.join()
        self.assertLess(time.monotonic() - begin, 30)
        state = self.journal().status(job)
        self.assertEqual((state["state"], state["result_sha256"]), ("cancelled", None))

    def test_timeout_kills_the_sandbox(self):
        job = "8" * 64
        self.queued(job, timeout=1)
        begin = time.monotonic()
        self.assertEqual(self.supervise(job, self.sleeper), 0)
        self.assertLess(time.monotonic() - begin, 30)
        state = self.journal().status(job)
        self.assertEqual(state["state"], "failed")
        self.assertIn("timed out after 1 s", state["error"])


if __name__ == "__main__":
    unittest.main()
