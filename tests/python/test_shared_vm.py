"""The app's one shared VM with several projects (TSUGUMI_TEST_SHARED_VM=1):
one Nix store for all, separate users,
views and processes per project, and stopping one project never stops
another's run."""

from __future__ import annotations

import json
import os
from contextlib import closing
from pathlib import Path

import pytest

from conftest import SharedVM, VMEnv, write_project
from harness.host import HostConnection
from projects import card
from projects import run_and_wait, start_run, wait_for_log, wait_run, write_nix_project as write
from harness.real_cards import ONE_MAIN, log, python_card
from tsugumi_client import RpcFailure, new_id, path, text
from Tsugumi.Data.LogStream import LogStream
from Tsugumi.Data.RunState import RunState
from Tsugumi.Host.EventPayload import EventPayload
from Tsugumi.Trace.CoreTraceRequest import CoreTraceRequestT
from Tsugumi.Trace.Mode import Mode

pytestmark = pytest.mark.skipif(
    os.environ.get("TSUGUMI_TEST_SHARED_VM") != "1",
    reason="needs the shared VM (TSUGUMI_TEST_SHARED_VM=1)")


HOLDING_MAIN = """import time

def run(input):
    print("HOLDING", flush=True)
    while True:
        time.sleep(60)
"""


def project(root, main, name):
    write(root, {"trigger": {"type": "trigger"}, "step": python_card(main)}, [card("start", "trigger"), card("step", "step")], [])
    return VMEnv(root).start()


def test_projects_share_the_store_but_not_users_views_or_processes(tmp_path):
    a = project(tmp_path / "a", ONE_MAIN, "a")
    b = project(tmp_path / "b", ONE_MAIN, "b")
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


def test_project_cannot_read_cache_credentials_or_change_daemon_trust(tmp_path):
    env = project(tmp_path / "cache-trust", ONE_MAIN, "cache-trust")

    try:
        hidden = env.exec(
            "test ! -e /run/tsugumi/binary-caches/netrc && "
            "test ! -e /run/tsugumi/binary-caches/nix.conf && echo hidden"
        )
        assert hidden.strip().endswith("hidden"), hidden
        probe = env.exec("echo probe > /tmp/cache-trust-probe; nix-store --add /tmp/cache-trust-probe")
        store_path = probe.strip().splitlines()[-1]
        assert store_path.startswith("/nix/store/"), probe
        attempt = env.exec(
            "nix-store --realise "
            "--option substituters https://unconfigured.invalid "
            "--option trusted-public-keys probe:BwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwc= "
            f"{store_path} 2>&1"
        )
        assert "ignoring untrusted substituter" in attempt, attempt
        assert "trusted-public-keys" in attempt and "restricted setting" in attempt, attempt
    finally:
        env.stop()


def test_explicit_sdk_build_uses_the_guest_nix_sandbox(tmp_path):
    root = tmp_path / "sdk-build"
    write(root, {"sdk": {"type": "code", "buildTimeoutSeconds": 180, "_nix": """
        { params, inputs }:
        let pkgs = import <nixpkgs> {};
        in {
          api = pkgs.runCommand "guest-sdk-sandbox-test" { allowSubstitutes = false; } ''
            if [ -e /persist ]; then
              echo "guest store disk is visible to the builder" >&2
              exit 1
            fi
            mkdir -p "$out/share/tsugumi/api"
            printf 'sandboxed' > "$out/share/tsugumi/api/result"
          '';
        }
        """}}, [card("sdk", "sdk")], [])
    env = VMEnv(root).start()
    try:
        with env.client() as c:
            result = c.call("EvaluateNixRequest", card=path("sdk"), build=True,
                            expression="(import ./. { inherit params inputs; }).api")
        assert result.failure is None, (text(result.failure.diagnostic), text(result.log))
        assert [(text(f.path), bytes(f.content)) for f in result.files] == [
            ("share/tsugumi/api/result", b"sandboxed")]
    finally:
        env.stop()


def test_stopping_one_project_keeps_the_other_running(tmp_path):
    slow = project(tmp_path / "slow", HOLDING_MAIN, "slow")
    other = project(tmp_path / "other", ONE_MAIN, "other")
    try:
        c = slow.client()
        run_id = start_run(c, "start")
        wait_for_log(c, run_id, "step", 1, lambda log: "HOLDING" in text(log.text or b""))
        other.stop()
        assert c.call("RunStatusRequest", runId=run_id).state not in (RunState.failed, RunState.succeeded, RunState.cancelled)
        c.call("RunCancelRequest", runId=run_id)
        assert wait_run(c, run_id).state == RunState.cancelled
        # The stopped project starts again in the same VM.
        other.start()
        _, st = run_and_wait(other.client(), "start")
        assert st.state == RunState.succeeded
    finally:
        slow.stop()
        other.stop()


def test_build_cpu_time_counts_the_shared_daemon(tmp_path):
    """The project's core cannot see the shared nix-daemon from its PID
    namespace; vmd answers the build client the CPU time of the daemon
    processes serving it, so the invocation counts a derivation that computes
    for four seconds."""
    root = tmp_path / "build-cpu"
    main = "import os\nfrom card import Output\nfrom t.Num import NumT\n\ndef run(input):\n" \
           "    assert os.path.exists(PROBE)\n    return Output(out=NumT(value=1))\n"
    let = ("  probe = pkgs.runCommand \"tsugumi-vm-build-cpu-" + new_id() + "\" { allowSubstitutes = false; } ''\n"
           "    end=$((SECONDS + 4))\n    while [ \"$SECONDS\" -lt \"$end\" ]; do :; done\n    touch \"$out\"\n  '';")
    write(root, {"trigger": {"type": "trigger"},
                 "step": python_card(main, let=let, values={"PROBE": "\"${probe}\""}, buildTimeoutSeconds=300)},
          [card("start", "trigger"), card("step", "step")], [])
    env = VMEnv(root).start()
    try:
        c = env.client()
        run_id, outcome = run_and_wait(c, "start")
        assert outcome.state == RunState.succeeded, log(c, run_id, "step", LogStream.build)
        record = c.call("RunRecordRequest", runId=run_id, card=path("step"), invocation=1).record
        assert record.summary.cpuMs is not None and record.summary.cpuMs >= 2500, record.summary.cpuMs
    finally:
        env.stop()


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
    root = tmp_path / "P"
    write(root, {"trigger": {"type": "trigger"}, "one": python_card(ONE_MAIN)},
         [c_("start", "trigger"), c_("one", "one")], [])
    p = VMEnv(root).start()
    copy = None
    try:
        rid, st = run_and_wait(p.client(), "start")
        assert st.state == RunState.succeeded, log(p.client(), rid, "one", 3)
        roots = p.exec(f"ls /nix/var/nix/gcroots/tsugumi/projects/{p.id}")
        assert "--" in roots and "tsugumi-card.json" in roots, roots
        # The folder of roots is vmd's: no project can create one for another.
        assert p.exec("stat -c %a /nix/var/nix/gcroots/tsugumi/projects").strip().splitlines()[-1] == "755"
        assert "denied" in p.exec("mkdir /nix/var/nix/gcroots/tsugumi/projects/someone-else 2>&1")
        # Reopened: evaluated again, nothing built.
        p.stop()
        p.start()
        rid, st = run_and_wait(p.client(), "start")
        assert st.state == RunState.succeeded
        assert build_outcome(p.client(), rid, "one")[Phase.build] == 3
        # The shared store's cleanup keeps rooted results.
        assert SharedVM.get().command("vm gc")[-1].startswith("ok ")
        rid, st = run_and_wait(p.client(), "start")
        assert build_outcome(p.client(), rid, "one")[Phase.build] == 3
        # A copy (another id) roots what it uses itself; the original's data
        # goes and the cleanup runs: the copy still builds nothing.
        p.stop()
        import shutil
        shutil.copytree(root, tmp_path / "P2", symlinks=True)
        copy = VMEnv(tmp_path / "P2").start()
        rid, st = run_and_wait(copy.client(), "start")
        assert build_outcome(copy.client(), rid, "one")[Phase.build] == 3
        assert "tsugumi-card.json" in copy.exec(f"ls /nix/var/nix/gcroots/tsugumi/projects/{copy.id}")
        copy.stop()
        SharedVM.get().command(f"delete {root}")
        assert SharedVM.get().command("vm gc")[-1].startswith("ok ")
        copy.start()
        rid, st = run_and_wait(copy.client(), "start")
        assert st.state == RunState.succeeded
        assert build_outcome(copy.client(), rid, "one")[Phase.build] == 3
    finally:
        p.stop()
        if copy:
            copy.stop()


def test_run_history_lives_in_the_vm_with_a_copy_in_the_project(tmp_path):
    root = tmp_path / "hist"
    env = project(root, ONE_MAIN, "hist")
    try:
        def listed():
            return [text(r.status.runId) for r in env.client().call("RunHistoryRequest", limit=100).runs]

        rid, st = run_and_wait(env.client(), "start")
        assert st.state == RunState.succeeded
        assert listed() == [rid]
        # It stays across restarts.
        env.stop()
        env.start()
        assert listed() == [rid]
        # Data deletion removes it; the project's copy brings the runs back.
        env.stop()
        assert env.vm.command(f"delete {root}")[-1] == "deleted"
        env.start()
        assert listed() == [rid]
    finally:
        env.stop()


def test_closed_projects_give_their_slot_back(tmp_path):
    """The VM has a fixed number of folder slots: projects opened one after
    another, each closed (stopped) before the next, never run out of them."""
    from projects import builtin_project
    for i in range(18):  # more than the 16 slots
        root = tmp_path / f"p{i}"
        builtin_project(root)
        env = VMEnv(root).start()
        try:
            c = env.client()
            assert c.call("ProjectInspectRequest").rev >= 0
        finally:
            env.stop()


def test_a_crashed_tsugumid_is_started_again_with_its_documents(tmp_path):
    """The project's tsugumid killed in the VM: the next start (the app's
    reconnection) starts a new one for the same project, which serves its
    documents as before."""
    from projects import builtin_project
    root = tmp_path / "p"
    builtin_project(root)
    env = VMEnv(root).start()
    try:
        rev = env.client().call("ProjectInspectRequest").rev
        env.crash()
        env.start()
        assert env.client().call("ProjectInspectRequest").rev == rev
    finally:
        env.stop()


@pytest.mark.parametrize("rpc_trace", [False, True])
def test_host_launches_guest_core_with_its_trace_grant(tmp_path, rpc_trace):
    root = tmp_path / "project"
    write_project(root)
    vm = SharedVM(rpc_trace=rpc_trace)
    try:
        env = VMEnv(root, vm=vm).start()
        with env.client() as core:
            if not rpc_trace:
                capability = (env.control / "execution-broker-capability").read_text()
                for mode in (None, Mode.Capture):
                    request = CoreTraceRequestT()
                    request.capability = capability
                    request.mode = mode
                    with pytest.raises(RpcFailure) as error:
                        core.wait_reply(core.send("CoreTraceRequest", request))
                    assert error.value.kind == "permission_denied"
                return

            ready = Path(vm.directory.name) / "host-core/host.ready"
            with closing(HostConnection(json.loads(ready.read_text()), trace=True)) as viewer:
                project_id = env.ready["project_id"]
                watched = viewer.call("HostTraceRequest", mode=Mode.Trace)
                assert {r.id.decode() for r in watched.recorders if r.available} == {project_id}
                snapshot = viewer.call("HostTraceRequest", source=project_id).snapshot
                core.call("ProjectActivityRequest")
                event = viewer.next_event()
                assert event.valueType == EventPayload.TraceChanged
                assert event.value.source.decode() == project_id
                delta = viewer.call("HostTraceRequest", source=project_id, generation=snapshot.generation,
                                    after=max((e.sequence for e in snapshot.entries or []), default=0)).snapshot
                assert any(e.messageType == b"ProjectActivityRequest" for e in delta.entries)
    finally:
        vm.quit()
