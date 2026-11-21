"""Workers in the real environment (TSUGUMI_TEST_CONTAINER=1, the shared VM):
a resident worker serves later runs (onRunStart starts each run), a Card
with File inputs keeps its worker, inputs stay read-only while the call's
output folder is staged inside the worker, reuseWorker = false gets a fresh
worker per call, and nothing of a call's descriptors is left over."""

from __future__ import annotations

import os
import re

import pytest

from harness.observations import decode_scalar, read_output
from conftest import real_environment
from projects import card, conn
from projects import put, run_and_wait, write
from test_cards_container import NUM, bash, load, log
from tsugumi_client import path, text
from Tsugumi.Data.CardState import CardState
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.RunState import RunState

pytestmark = pytest.mark.skipif(os.environ.get("TSUGUMI_TEST_CONTAINER") != "1",
                                reason="needs the shared VM (TSUGUMI_TEST_CONTAINER=1)")

CPP_NIX = """{ pkgs, inputs, params }:
let
  sdk = inputs.tsugumi.sdk { languages = [ "cpp" ]; };
in
{
  run = inputs.tsugumi.cppCard { inherit sdk; src = ./src; };
}
"""
DOC = ('''namespace t; table Doc { value:string; }
table Outputs { doc:Doc (required, tsugumi: "file"); }
'''
       '''root_type Outputs;
attribute "tsugumi";
''')

COUNT = '''
#include <card.hh>

#include <random>
#include <string>

// Identifies the process (PIDs repeat across sandboxes: each has its own PID namespace).
static const unsigned worker = std::random_device{}();
static int runs = 0, calls = 0;

auto onRunStart(const Card::Inputs& inputs) -> void {
  ++runs;
  calls = 0;
}

auto run(const Card::Inputs& inputs, tsugumi::Report& report) -> Card::Output {
  ++calls;
  report.text("worker " + std::to_string(worker) + " runs " + std::to_string(runs) + " calls " +
                               std::to_string(calls));
  Card::Output out;
  out.out.emplace().value = runs * 1000 + calls;
  return out;
}
'''

# Reads its File input, keeps every line it saw (a stream kept by the
# resident worker) and must not be able to change the input.
READ = '''
#include <card.hh>

#include <fstream>
#include <string>
#include <vector>

static std::vector<std::string> lines;

auto run(const Card::Inputs& inputs, tsugumi::Report& report) -> Card::Output {
  // No folder descriptor of this or an earlier call is left (this Card has no File outputs).
  if (std::ifstream("/proc/self/fd/900")) throw tsugumi::Failure("worker_failed", "a passed descriptor was left open");
  std::ifstream in(inputs.doc.value);
  std::string line;
  if (!std::getline(in, line)) throw tsugumi::Failure("input_missing", "cannot read " + inputs.doc.value);
  lines.push_back(line);
  if (std::ofstream(inputs.doc.value, std::ios::app) << "x" << std::flush)
    throw tsugumi::Failure("output_invalid", "the inputs file was writable");
  report.text(std::to_string(lines.size()) + " lines");
  Card::Output out;
  out.out.emplace().value = lines.size();
  return out;
}
'''

# Without reuseWorker (the default): a new process per call, each starting with calls = 1.
FRESH = '''
#include <card.hh>

#include <random>
#include <string>

static const unsigned worker = std::random_device{}();
static int calls = 0;

auto run(const Card::Inputs& inputs, tsugumi::Report& report) -> Card::Output {
  ++calls;
  uint64_t n = 0;
  if (const auto* v = inputs.x.value.Asinit()) n = v->value;
  else if (const auto* v = inputs.x.value.Asagain()) n = v->value;
  report.log("worker " + std::to_string(worker) + " calls " + std::to_string(calls));
  Card::Output out;
  if (n > 0) out.again.emplace().value = n - 1;
  return out;
}
'''


@pytest.fixture(scope="module")
def env(tmp_path_factory):
    root = tmp_path_factory.mktemp("workers") / "p"
    write(root, {"trigger": {"type": "trigger"}}, [card("start", "trigger")], [])
    e = real_environment(root)
    yield e
    e.cleanup()


def display(c, run_id, card_id):
    shown = c.call("RunRecordRequest", runId=run_id, card=path(card_id)).record.display
    return text(shown.value.text) if shown is not None and shown.valueType == 1 else ""


def invocations(c, run_id, card_id):
    return c.call("RunInvocationsRequest", runId=run_id, card=path(card_id)).records


def test_resident_worker_serves_later_runs(env):
    c = load(env, {"trigger": {"type": "trigger"},
                   "count": {"type": "code", "language": "cpp", "reuseWorker": True, "_outputs": NUM, "_nix": CPP_NIX}},
             [card("start", "trigger"), card("count", "count")], [])
    put(env.project / "definitions" / "count" / "src" / "card.cc", COUNT)
    from test_cards_container import reload
    reload(c)
    first, st = run_and_wait(c, "start")
    assert st.state == RunState.succeeded, log(c, first, "count", 2)
    second, st = run_and_wait(c, "start")
    assert st.state == RunState.succeeded, log(c, second, "count", 2)
    a, b = display(c, first, "count"), display(c, second, "count")
    pa, pb = re.search(r"worker (\d+)", a).group(1), re.search(r"worker (\d+)", b).group(1)
    # The same process; onRunStart started the second run (calls back to 1).
    assert pa == pb and a.endswith("runs 1 calls 1") and b.endswith("runs 2 calls 1"), (a, b)
    assert not invocations(c, first, "count")[0].workerReused
    assert invocations(c, second, "count")[0].workerReused


def test_file_inputs_keep_the_worker_and_stay_read_only(env):
    # The bash run writes its File output in the worker staging area.
    # No directory descriptor from outside the sandbox is passed in.
    make = bash('test -d "$TSUGUMI_OUTPUT_DIR" && test ! -e /proc/self/fd/900 && '
                'echo "line $TSUGUMI_INVOCATION-$RANDOM" > "$TSUGUMI_OUTPUT_DIR/a.txt" && '
                'printf \'{"doc":{"value":"%s"}}\' "$TSUGUMI_OUTPUT_DIR/a.txt" > "$TSUGUMI_OUTPUT"', outputs=DOC)
    c = load(env, {"trigger": {"type": "trigger"}, "make": make,
                   "read": {"type": "code", "language": "cpp", "inputs": [{"name": "doc"}], "reuseWorker": True, "_outputs": NUM, "_nix": CPP_NIX}},
             [card("start", "trigger"), card("make", "make"), card("read", "read")],
             [conn("make", "doc", "read", "doc")])
    put(env.project / "definitions" / "read" / "src" / "card.cc", READ)
    from test_cards_container import reload
    reload(c)
    first, st = run_and_wait(c, "start")
    detail = {k: c.call("RunRecordRequest", runId=first, card=path(k)).status for k in ("make", "read")}
    assert st.state == RunState.succeeded, ({k: v.failure and text(v.failure.diagnostic) for k, v in detail.items()},
                                            log(c, first, "make", 2), log(c, first, "read", 2))
    second, st = run_and_wait(c, "start")
    assert st.state == RunState.succeeded, log(c, second, "read", 2)
    # Another input path (another run's output) and the same worker: the stream goes on.
    assert display(c, second, "read") == "2 lines"
    assert invocations(c, second, "read")[0].workerReused
    # The output path recorded is the real one, inside the run's tree.
    rec = c.call("RunRecordRequest", runId=second, card=path("make"))
    assert rec.status.state == CardState.succeeded


def test_fresh_worker_per_call(env):
    nix = CPP_NIX
    out = ('''namespace t; table Num { value:ulong; }
table Outputs { again:Num; }
root_type Outputs;
''')
    seed = bash('echo \'{"out":{"value":3}}\' > "$TSUGUMI_OUTPUT"')
    c = load(env, {"trigger": {"type": "trigger"}, "seed": seed,
                   "loop": {"type": "merge", "inputs": [{"name": "init"}, {"name": "again"}], "maxRuns": 10},
                   "fresh": {"type": "code", "language": "cpp", "inputs": [{"name": "x"}], "maxRuns": 10,  # no reuseWorker: the default
                             "_outputs": out, "_nix": nix}},
             [card("start", "trigger"), card("seed", "seed"), card("loop", "loop"), card("fresh", "fresh")],
             [conn("seed", "out", "loop", "init"), conn("loop", "out", "fresh", "x"), conn("fresh", "again", "loop", "again")])
    put(env.project / "definitions" / "fresh" / "src" / "card.cc", FRESH)
    from test_cards_container import reload
    reload(c)
    run_id, st = run_and_wait(c, "start")
    assert st.state == RunState.succeeded, log(c, run_id, "fresh", 2)
    recs = invocations(c, run_id, "fresh")
    assert len(recs) == 4
    workers = set()
    for inv in range(1, 5):
        line = log(c, run_id, "fresh", 2, inv)
        m = re.search(r"worker (\d+) calls (\d+)", line)
        assert m and m.group(2) == "1", line
        workers.add(m.group(1))
    assert len(workers) == 4 and not any(r.workerReused for r in recs)


def test_evaluator_errors_and_timeouts_leave_tsugumid_working(env):
    """The evaluator runs inside tsugumid: endless recursion is an error of
    the Card (never a crash), buildTimeoutSeconds stops a build, and the next
    Card evaluates and runs as before."""
    loop_nix = '''{ pkgs, inputs, params }:
let f = x: f (x + 1); in { run = f 0; }
'''
    slow_nix = ('''{ pkgs, inputs, params }:
{ run = pkgs.runCommand "tsugumi-test-slow-${toString builtins.currentTime}" { } '''
                '''"sleep 60; mkdir -p $out/lib"; }
''')
    ok = bash('echo \'{"out":{"value":1}}\' > "$TSUGUMI_OUTPUT"')
    c = load(env, {"trigger": {"type": "trigger"},
                   "loop": {"type": "code", "language": "bash", "_outputs": NUM, "_nix": loop_nix},
                   "slow": {"type": "code", "language": "bash", "buildTimeoutSeconds": 3, "_outputs": NUM, "_nix": slow_nix}, "ok": ok},
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


# Reach 1.1.1.1:80 over TCP (bash's /dev/tcp, no DNS).
REACH = 'if (exec 3<>/dev/tcp/1.1.1.1/80) 2>/dev/null; then reachable=true; else reachable=false; fi'


def number(c, run_id, card_id):
    rec = c.call("RunRecordRequest", runId=run_id, card=path(card_id))
    assert rec.status.state == CardState.succeeded, rec.status.failure and text(rec.status.failure.diagnostic)
    return log(c, run_id, card_id)


def test_network_is_off_unless_the_card_allows_it(env):
    """The network setting applies to both preRun and the Card body."""
    outputs = "table Reach { before:bool; body:bool; } table Outputs { out:Reach (required); } root_type Outputs;"
    body = REACH + """
printf '{"out":{"before":%s,"body":%s}}' "$PRE_REACH" "$reachable" > "$TSUGUMI_OUTPUT"
"""
    cards = {"trigger": {"type": "trigger"}}
    for name, settings in {"closed": {}, "closed_unsandboxed": {"sandbox": {"filesystem": False}},
                           "open": {"grants": {"network": True}}, "open_unsandboxed": {"grants": {"network": True}, "sandbox": {"filesystem": False}}}.items():
        cards[name] = bash(body, inputs=["go"], outputs=outputs, recordIO=True,
                           pre=REACH + '; export PRE_REACH="$reachable"', **settings)
    names = [n for n in cards if n != "trigger"]
    c = load(env, cards, [card("start", "trigger")] + [card(n, n) for n in names],
             [conn("start", "out", n, "go") for n in names])
    run_id, status = run_and_wait(c, "start")
    assert status.state == RunState.succeeded
    for name in names:
        payload = read_output(c, run_id, card=name)
        expected = name.startswith("open")
        assert bool(decode_scalar(payload, "bool", field=0)) == expected, name
        assert bool(decode_scalar(payload, "bool", field=1)) == expected, name


def test_mac_folders_are_read_only_unless_written(env, tmp_path):
    """Experimental host folders: a folder of the Mac named by a Card is
    shared with its project only, at /host/<name> in the worker, read-only
    unless the Card writes there."""
    shared = tmp_path / "mac-folder"
    shared.mkdir()
    (shared / "hello.txt").write_text("from the Mac")
    read = bash('cat /host/data/hello.txt; if echo x > /host/data/new.txt 2>/dev/null; then echo wrote; else echo read-only; fi; '
                'echo \'{"out":{"value":1}}\' > "$TSUGUMI_OUTPUT"', inputs=["go"],
                grants={"hostPaths": [{"path": str(shared), "name": "data"}]})
    write_ = bash('echo made > /host/out/made.txt && echo \'{"out":{"value":1}}\' > "$TSUGUMI_OUTPUT"', inputs=["go"],
                  grants={"hostPaths": [{"path": str(shared), "name": "out", "write": True}]})
    other = bash('ls /host 2>&1 || true; echo \'{"out":{"value":1}}\' > "$TSUGUMI_OUTPUT"', inputs=["go"])
    load(env, {"trigger": {"type": "trigger"}, "read": read, "write": write_, "other": other},
         [card("start", "trigger"), card("read", "read"), card("write", "write"),
          card("other", "other")], [conn("start", "out", n, "go") for n in ("read", "write", "other")])
    # The share is set when the project's environment starts.
    env.stop()
    env.start()
    c = env.client()
    run_id, st = run_and_wait(c, "start")
    out = number(c, run_id, "read")
    assert "from the Mac" in out and "read-only" in out, out
    number(c, run_id, "write")
    assert (shared / "made.txt").read_text().strip() == "made"
    assert not (shared / "new.txt").exists()
    assert "data" not in number(c, run_id, "other")  # a Card that names no folder sees none
