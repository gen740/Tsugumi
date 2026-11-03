"""The app's one shared VM with several projects (TSUGUMI_TEST_CONTAINER=1,
not with TSUGUMI_TEST_ENV=container): one Nix store for all, separate users,
views and processes per project, and stopping one project never stops
another's run."""

from __future__ import annotations

import os
import time

import pytest

from conftest import SharedVM, VMEnv
from projects import card
from test_cards_container import bash, make, run, log
from test_run_history import copy_runs, wait_copy
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


CPP_ONE = r"""
#include <card.hh>

auto run(Card::Input&) -> Card::Output {
  Card::Output out;
  out.out.emplace().value = 1;
  return out;
}
"""


def build_outcome(c, run_id, card_id):
    """Outcome of the build stage of the Card's first invocation (2 done: built, 3 cached: in the store)."""
    rec = c.call("RunInvocationsRequest", runId=run_id, card=path(card_id)).records[0]
    return {s.phase: s.outcome for s in rec.stages}


def test_build_roots_across_reopen_store_cleanup_and_copies(tmp_path):
    """Results stay usable without rebuilding after a reopen, the shared
    store's cleanup, and for a copy of the project (its own id and roots)
    after the original's data is deleted. Project root folders are created
    by vmd only."""
    from Tsugumi.Data.Phase import Phase
    from projects import card as c_
    nix = ("{ pkgs, inputs, params }:\nlet\n  sdk = inputs.tsugumi.sdk { languages = [ \"cpp\" ]; };\nin\n"
           "{\n  run = inputs.tsugumi.cppCard { inherit sdk; src = ./src; };\n}\n")
    from test_cards_container import NUM
    from test_regressions import put
    root = tmp_path / "P"
    make(root, {"trigger": {"template": "trigger"}, "one": {"language": "cpp", "_outputs": NUM, "_nix": nix}},
         [c_("begin", "trigger"), c_("one", "one")], [])
    put(root / "definitions" / "one" / "src" / "card.cc", CPP_ONE)
    p = VMEnv(root, "P").start()
    copy = None
    try:
        rid, st = run(p.client(), "begin")
        assert st.state == RunState.succeeded, log(p.client(), rid, "one", 3)
        roots = p.exec(f"ls /nix/var/nix/gcroots/tsugumi/projects/{p.id}")
        assert "--" in roots and "tsugumi-card.json" in roots, roots
        # The folder of roots is vmd's: no project can create one for another.
        assert p.exec("stat -c %a /nix/var/nix/gcroots/tsugumi/projects").strip().splitlines()[-1] == "755"
        assert "denied" in p.exec("mkdir /nix/var/nix/gcroots/tsugumi/projects/someone-else 2>&1")
        # Reopened: evaluated again, nothing built.
        p.stop()
        p.start()
        rid, st = run(p.client(), "begin")
        assert st.state == RunState.succeeded
        assert build_outcome(p.client(), rid, "one")[Phase.build] == 3
        # The shared store's cleanup keeps rooted results.
        assert SharedVM.get().command("vm gc")[-1].startswith("ok ")
        rid, st = run(p.client(), "begin")
        assert build_outcome(p.client(), rid, "one")[Phase.build] == 3
        # A copy (another id) roots what it uses itself; the original's data
        # goes and the cleanup runs: the copy still builds nothing.
        p.stop()
        import shutil
        shutil.copytree(root, tmp_path / "P2", symlinks=True)
        copy = VMEnv(tmp_path / "P2", "P2").start()
        rid, st = run(copy.client(), "begin")
        assert build_outcome(copy.client(), rid, "one")[Phase.build] == 3
        assert "tsugumi-card.json" in copy.exec(f"ls /nix/var/nix/gcroots/tsugumi/projects/{copy.id}")
        copy.stop()
        SharedVM.get().command(f"delete {root}")
        assert SharedVM.get().command("vm gc")[-1].startswith("ok ")
        copy.start()
        rid, st = run(copy.client(), "begin")
        assert st.state == RunState.succeeded
        assert build_outcome(copy.client(), rid, "one")[Phase.build] == 3
    finally:
        p.stop()
        if copy:
            copy.stop()


def test_run_history_lives_in_the_vm_with_a_copy_in_the_project(tmp_path):
    root = tmp_path / "hist"
    env = project(root, 'echo \'{"out":{"value":1}}\' > "$TSUGUMI_OUTPUT"', "hist")
    try:
        def listed():
            return [text(r.status.runId) for r in env.client().call("RunHistoryRequest", limit=100).runs]

        rid, st = run(env.client(), "begin")
        assert st.state == RunState.succeeded
        # The database is on the VM's disk, seen by the project only as /history.
        assert "history.sqlite" in env.exec("ls /history")
        assert not (root / ".tsugumi" / "history.sqlite").exists()
        assert wait_copy(root, 1) == [rid]
        assert env.id in env.vm.command("vm histories")[-1]
        # It stays across restarts.
        env.stop()
        env.start()
        assert listed() == [rid]
        # Data deletion removes it; the project's copy brings the runs back.
        env.stop()
        assert env.vm.command(f"delete {root}")[-1] == "deleted"
        assert env.id not in env.vm.command("vm histories")[-1]
        env.start()
        assert listed() == [rid]
        assert copy_runs(root) == [rid]
    finally:
        env.stop()
