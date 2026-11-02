"""The app's one shared VM with several projects (TSUGUMI_TEST_CONTAINER=1,
not with TSUGUMI_TEST_ENV=container): one Nix store for all, separate users,
views and processes per project, and stopping one project never stops
another's run."""

from __future__ import annotations

import os
import time

import pytest

from conftest import VMEnv
from projects import card
from test_cards_container import bash, make, run, log
from tsugumi_client import new_id, path, text
from Tsugumi.Data.RunState import RunState

pytestmark = pytest.mark.skipif(
    os.environ.get("TSUGUMI_TEST_CONTAINER") != "1" or os.environ.get("TSUGUMI_TEST_ENV") == "container",
    reason="needs the shared VM (TSUGUMI_TEST_CONTAINER=1)")


def project(root, script, name):
    make(root, {"trigger": {"template": "trigger"}, "step": bash(script)}, [card("begin", "trigger"), card("step", "step")], [])
    return VMEnv(root, name).start()


def test_projects_share_the_store_but_not_users_views_or_processes(tmp_path):
    a = project(tmp_path / "a", 'echo \'{"out":{"value":1}}\' > "$TSUGUMI_OUTPUT"', "a")
    b = project(tmp_path / "b", 'echo \'{"out":{"value":1}}\' > "$TSUGUMI_OUTPUT"', "b")
    try:
        # Separate users.
        ua, ub = a.exec("id -u").strip().splitlines()[-1], b.exec("id -u").strip().splitlines()[-1]
        assert ua != ub and int(ua) >= 20001 and int(ub) >= 20001
        # Each sees only its own project: no share, no store disk, no other project.
        for env, other in ((a, b), (b, a)):
            seen = env.exec("ls -a /mnt/slots 2>/dev/null | wc -l; [ -e /persist/nix ] && echo persist; "
                            "ls /project; cat /control/server.ready >/dev/null && echo own-control")
            assert "persist" not in seen and "own-control" in seen and "tsugumi.json" in seen, seen
            assert seen.strip().splitlines()[0].strip() == "0", seen  # no slots at all
            assert other.id not in env.exec("cat /proc/[0-9]*/cmdline 2>/dev/null | tr '\\0' ' '")
        # Only its own processes (PID namespace): the other's tsugumid is not there.
        procs = a.exec("for f in /proc/[0-9]*/comm; do cat $f; done | sort | uniq -c")
        assert procs.count("tsugumid") == 1, procs
        # One store: what A adds, B sees (same path, no second copy).
        added = a.exec("echo shared-$RANDOM > /tmp/x && nix-store --add /tmp/x").strip().splitlines()[-1]
        assert added.startswith("/nix/store/"), added
        assert b.exec(f"test -e {added} && echo visible").strip().endswith("visible")
    finally:
        a.stop()
        b.stop()


def test_stopping_one_project_keeps_the_other_running(tmp_path):
    slow = project(tmp_path / "slow", 'sleep 6; echo \'{"out":{"value":1}}\' > "$TSUGUMI_OUTPUT"', "slow")
    other = project(tmp_path / "other", 'echo \'{"out":{"value":1}}\' > "$TSUGUMI_OUTPUT"', "other")
    try:
        c = slow.client()
        snap = c.call("ProjectInspectRequest")
        acc = c.call("RunRequest", baseRev=snap.rev, opId=new_id(), origin="t", card=path("begin"))
        run_id = text(acc.run.runId)
        # Wait until the slow Card runs, then stop the other project.
        # Wait until the slow Card's process runs (seen from the project's own view).
        deadline = time.time() + 600
        while "sleep" not in slow.exec("for f in /proc/[0-9]*/comm; do cat $f; done 2>/dev/null"):
            assert time.time() < deadline, "the Card never started"
            assert c.call("RunStatusRequest", runId=run_id).state not in (RunState.failed, RunState.succeeded)
            time.sleep(0.2)
        other.stop()
        while c.call("RunStatusRequest", runId=run_id).state not in (RunState.succeeded, RunState.failed, RunState.cancelled):
            time.sleep(0.2)
        assert c.call("RunStatusRequest", runId=run_id).state == RunState.succeeded, log(c, run_id, "step", 2)
        # The stopped project starts again in the same VM.
        other.start()
        _, st = run(other.client(), "begin")
        assert st.state == RunState.succeeded
    finally:
        slow.stop()
        other.stop()
