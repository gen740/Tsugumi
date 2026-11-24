"""Workers in the real environment (TSUGUMI_TEST_SHARED_VM=1, the shared VM):
a resident worker serves later runs and keeps its state, a Card with Path
inputs keeps its worker while its inputs stay read-only, reuseWorker = false
gets a fresh worker per call, the evaluator's errors leave tsugumid working,
the network grant and granted Mac folders reach the Card as configured."""

from __future__ import annotations

import os
import re

import pytest

from harness.observations import decode_scalar, read_output
from harness.real_cards import NUM, ONE_MAIN, TXT, log, python_card, txt_main
from conftest import real_environment
from projects import card, conn, run_and_wait, write_nix_project as write
from test_cards_shared_vm import load, shown
from tsugumi_client import path, text
from Tsugumi.Data.CardState import CardState
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.RunState import RunState

pytestmark = pytest.mark.skipif(os.environ.get("TSUGUMI_TEST_SHARED_VM") != "1",
                                reason="needs the shared VM (TSUGUMI_TEST_SHARED_VM=1)")

# Identifies the process (PIDs repeat across sandboxes: each has its own PID namespace).
COUNT_MAIN = """import random
from card import Output
from t.Txt import TxtT

WORKER = random.getrandbits(32)
calls = 0

def run(input):
    global calls
    calls += 1
    return Output(out=TxtT(value=f"worker {WORKER} calls {calls}"))
"""

# Without reuseWorker (the default): a new process per call, each starting with calls = 1.
FRESH_MAIN = """import random
from card import Output
from t.Num import NumT

WORKER = random.getrandbits(32)
calls = 0

def run(input):
    global calls
    calls += 1
    print(f"worker {WORKER} calls {calls}", flush=True)
    n = input.x.value
    return Output(again=NumT(value=n - 1) if n > 0 else None)
"""

SEED_MAIN = ONE_MAIN.replace("value=1", "value=3")


@pytest.fixture(scope="module")
def env(tmp_path_factory):
    root = tmp_path_factory.mktemp("workers") / "p"
    write(root, {"trigger": {"type": "trigger"}}, [card("start", "trigger")], [])
    e = real_environment(root)
    yield e
    e.cleanup()


def invocations(c, run_id, card_id):
    return c.call("RunInvocationsRequest", runId=run_id, card=path(card_id)).records


def test_resident_worker_serves_later_runs(env):
    c = load(env, {"trigger": {"type": "trigger"},
                   "count": python_card(COUNT_MAIN, outputs=TXT, reuseWorker=True, recordIO=True)},
             [card("start", "trigger"), card("count", "count")], [])
    first, st = run_and_wait(c, "start")
    assert st.state == RunState.succeeded, log(c, first, "count", 2)
    second, st = run_and_wait(c, "start")
    assert st.state == RunState.succeeded, log(c, second, "count", 2)
    a, b = shown(c, first, "count"), shown(c, second, "count")
    pa, pb = re.search(r"worker (\d+)", a).group(1), re.search(r"worker (\d+)", b).group(1)
    # The same process, and its state carries across the runs.
    assert pa == pb and a.endswith("calls 1") and b.endswith("calls 2"), (a, b)
    assert not invocations(c, first, "count")[0].workerReused
    assert invocations(c, second, "count")[0].workerReused


def test_fresh_worker_per_call(env):
    out = "namespace t; table Num { value:ulong; } table Outputs { again:Num; } root_type Outputs;"
    c = load(env, {"trigger": {"type": "trigger"}, "seed": python_card(SEED_MAIN),
                   "loop": {"type": "merge", "inputs": [{"name": "init"}, {"name": "again"}], "maxRuns": 10},
                   # No reuseWorker: the default.
                   "fresh": python_card(FRESH_MAIN, inputs=["x"], outputs=out, maxRuns=10)},
             [card("start", "trigger"), card("seed", "seed"), card("loop", "loop"), card("fresh", "fresh")],
             [conn("seed", "out", "loop", "init"), conn("loop", "out", "fresh", "x"), conn("fresh", "again", "loop", "again")])
    run_id, st = run_and_wait(c, "start")
    assert st.state == RunState.succeeded, log(c, run_id, "fresh", 2)
    recs = invocations(c, run_id, "fresh")
    assert len(recs) == 4
    workers = set()
    for inv in range(1, 5):
        line = log(c, run_id, "fresh", 1, inv)
        m = re.search(r"worker (\d+) calls (\d+)", line)
        assert m and m.group(2) == "1", line
        workers.add(m.group(1))
    assert len(workers) == 4 and not any(r.workerReused for r in recs)


def test_evaluator_errors_and_timeouts_leave_tsugumid_working(env):
    """The evaluator runs inside tsugumid: endless recursion is an error of
    the Card (never a crash), buildTimeoutSeconds stops a build, and the next
    Card evaluates and runs as before."""
    loop_nix = '''{ params, inputs }:
let f = x: f (x + 1); in { run = f 0; }
'''
    slow_nix = ('''{ params, inputs }:
let pkgs = import <nixpkgs> { }; in
{ run = pkgs.runCommand "tsugumi-test-slow" { } '''
                '''"sleep 60; mkdir -p $out/lib"; }
''')
    c = load(env, {"trigger": {"type": "trigger"},
                   "loop": {"type": "code", "_outputs": NUM, "_nix": loop_nix},
                   "slow": {"type": "code", "buildTimeoutSeconds": 3, "_outputs": NUM, "_nix": slow_nix},
                   "ok": python_card(ONE_MAIN)},
             [card("start", "trigger"), card("loop", "loop"), card("slow", "slow"), card("ok", "ok")], [])
    run_id, st = run_and_wait(c, "start")
    rec = {k: c.call("RunRecordRequest", runId=run_id, card=path(k)).status for k in ("loop", "slow", "ok")}
    assert rec["loop"].state == CardState.failed
    diag = text(rec["loop"].failure.diagnostic)
    assert "recursion" in diag or "max-call-depth" in diag or "stack overflow" in diag, diag
    assert rec["slow"].state == CardState.failed and rec["slow"].failure.kind == FailureKind.timeout, text(rec["slow"].failure.diagnostic)
    assert rec["ok"].state == CardState.succeeded
    # tsugumid and its evaluator still work.
    c.call("ProjectInspectRequest")
    run_id, st = run_and_wait(c, "ok")
    assert c.call("RunRecordRequest", runId=run_id, card=path("ok")).status.state == CardState.succeeded


# Reach 1.1.1.1:80 over TCP (no DNS).
REACH_MAIN = """import socket
from card import Output
from t.Reach import ReachT

def run(input):
    try:
        socket.create_connection(("1.1.1.1", 80), timeout=3).close()
        reachable = True
    except OSError:
        reachable = False
    return Output(out=ReachT(reachable=reachable))
"""


def test_network_is_off_unless_the_card_allows_it(env):
    outputs = "namespace t; table Reach { reachable:bool; } table Outputs { out:Reach (required); } root_type Outputs;"
    cards = {"trigger": {"type": "trigger"}}
    for name, settings in {"closed": {}, "closed_unsandboxed": {"sandbox": {"filesystem": False}},
                           "open": {"grants": {"network": True}},
                           "open_unsandboxed": {"grants": {"network": True}, "sandbox": {"filesystem": False}}}.items():
        cards[name] = python_card(REACH_MAIN, inputs=["go"], outputs=outputs, recordIO=True, **settings)
    names = [n for n in cards if n != "trigger"]
    c = load(env, cards, [card("start", "trigger")] + [card(n, n) for n in names],
             [conn("start", "out", n, "go") for n in names])
    run_id, status = run_and_wait(c, "start")
    assert status.state == RunState.succeeded
    for name in names:
        payload = read_output(c, run_id, card=name)
        assert bool(decode_scalar(payload, "bool", field=0)) == name.startswith("open"), name


GRANTS_BODY = """
    import os
    found = []
    data, out = os.environ.get("TSUGUMI_GRANT_DATA"), os.environ.get("TSUGUMI_GRANT_OUT")
    if data:
        found.append(open(os.path.join(data, "hello.txt")).read())
        try:
            open(os.path.join(data, "new.txt"), "w").write("x")
            found.append("wrote")
        except OSError:
            found.append("read-only")
    if out:
        open(os.path.join(out, "made.txt"), "w").write("made")
    if not data and not out:
        found.append("no folder" if not os.path.exists("/host") or not os.listdir("/host") else "folders visible")
    RESULT = ";".join(found)"""


def test_mac_folders_are_read_only_unless_written(env, tmp_path):
    """A Card receives each granted Mac folder through its environment.
    A grant is read-only by default and allows writes only if requested."""
    shared = tmp_path / "mac-folder"
    shared.mkdir()
    (shared / "hello.txt").write_text("from the Mac")
    probe = txt_main(GRANTS_BODY)
    read = python_card(probe, outputs=TXT, inputs=["go"], recordIO=True,
                       grants={"hostPaths": [{"path": str(shared), "name": "data"}]})
    write_ = python_card(probe, outputs=TXT, inputs=["go"], recordIO=True,
                         grants={"hostPaths": [{"path": str(shared), "name": "out", "write": True}]})
    other = python_card(probe, outputs=TXT, inputs=["go"], recordIO=True)
    load(env, {"trigger": {"type": "trigger"}, "read": read, "write": write_, "other": other},
         [card("start", "trigger"), card("read", "read"), card("write", "write"),
          card("other", "other")], [conn("start", "out", n, "go") for n in ("read", "write", "other")])
    # The share is set when the project's environment starts.
    env.stop()
    env.start()
    c = env.client()
    run_id, st = run_and_wait(c, "start")
    assert st.state == RunState.succeeded, {n: log(c, run_id, n, 2) for n in ("read", "write", "other")}
    assert shown(c, run_id, "read") == "from the Mac;read-only"
    assert (shared / "made.txt").read_text().strip() == "made"
    assert not (shared / "new.txt").exists()
    assert shown(c, run_id, "other") == "no folder"  # a Card that names no folder sees none
