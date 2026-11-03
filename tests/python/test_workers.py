"""Workers in the real environment (TSUGUMI_TEST_CONTAINER=1, the shared VM):
a resident worker serves later runs (onRunStart starts each run), a Card
with File inputs keeps its worker, inputs stay read-only while the call's
output folder arrives as a descriptor, reuseWorker = false gets a fresh
worker per call, and nothing of a call's descriptors is left over."""

from __future__ import annotations

import os
import re

import pytest

from conftest import real_environment
from projects import card, conn
from test_cards_container import NUM, bash, load, log, run
from test_regressions import put, write
from tsugumi_client import path, text
from Tsugumi.Data.CardState import CardState
from Tsugumi.Data.RunState import RunState

pytestmark = pytest.mark.skipif(os.environ.get("TSUGUMI_TEST_CONTAINER") != "1" or os.environ.get("TSUGUMI_TEST_ENV") == "container",
                                reason="needs the shared VM (TSUGUMI_TEST_CONTAINER=1)")

CPP_NIX = """{ pkgs, inputs, params }:
let
  sdk = inputs.tsugumi.sdk { languages = [ "cpp" ]; };
in
{
  run = inputs.tsugumi.cppCard { inherit sdk; src = ./src; };
}
"""
DOC = ("namespace t; table Doc { value:string; }\ntable Outputs { doc:Doc (required, tsugumi: \"file\"); }\n"
       "root_type Outputs;\nattribute \"tsugumi\";\n")

COUNT = r"""
#include <card.hh>

#include <random>
#include <string>

// Identifies the process (PIDs repeat across sandboxes: each has its own PID namespace).
static const unsigned worker = std::random_device{}();
static int runs = 0, calls = 0;

auto onRunStart() -> void {
  ++runs;
  calls = 0;
}

auto run(Card::Input&) -> Card::Output {
  ++calls;
  tsugumi::visualization::text("worker " + std::to_string(worker) + " runs " + std::to_string(runs) + " calls " +
                               std::to_string(calls));
  Card::Output out;
  out.out.emplace().value = runs * 1000 + calls;
  return out;
}
"""

# Reads its File input, keeps every line it saw (a stream kept by the
# resident worker) and must not be able to change the input.
READ = r"""
#include <card.hh>

#include <fstream>
#include <string>
#include <vector>

static std::vector<std::string> lines;

auto run(Card::Input& input) -> Card::Output {
  // No folder descriptor of this or an earlier call is left (this Card has no File outputs).
  if (std::ifstream("/proc/self/fd/900")) throw tsugumi::Failure("worker_failed", "a passed descriptor was left open");
  std::ifstream in(input.doc.value);
  std::string line;
  if (!std::getline(in, line)) throw tsugumi::Failure("input_missing", "cannot read " + input.doc.value);
  lines.push_back(line);
  if (std::ofstream(input.doc.value, std::ios::app) << "x" << std::flush)
    throw tsugumi::Failure("output_invalid", "the input file was writable");
  tsugumi::visualization::text(std::to_string(lines.size()) + " lines");
  Card::Output out;
  out.out.emplace().value = lines.size();
  return out;
}
"""

# reuseWorker = false: a new process per call, each starting with calls = 1.
FRESH = r"""
#include <card.hh>

#include <random>
#include <string>

static const unsigned worker = std::random_device{}();
static int calls = 0;

auto run(Card::Input& input) -> Card::Output {
  ++calls;
  uint64_t n = 0;
  if (const auto* v = input.x.value.Asinit()) n = v->value;
  else if (const auto* v = input.x.value.Asagain()) n = v->value;
  tsugumi::log("worker " + std::to_string(worker) + " calls " + std::to_string(calls));
  Card::Output out;
  if (n > 0) out.again.emplace().value = n - 1;
  return out;
}
"""


@pytest.fixture(scope="module")
def env(tmp_path_factory):
    root = tmp_path_factory.mktemp("workers") / "p"
    write(root, {"trigger": {"template": "trigger"}}, [card("begin", "trigger")], [])
    e = real_environment(root, "tsugumi-test-workers")
    yield e
    e.cleanup()


def display(c, run_id, card_id):
    shown = c.call("RunRecordRequest", runId=run_id, card=path(card_id)).record.display
    return text(shown.value.text) if shown is not None and shown.valueType == 1 else ""


def invocations(c, run_id, card_id):
    return c.call("RunInvocationsRequest", runId=run_id, card=path(card_id)).records


def test_resident_worker_serves_later_runs(env):
    c = load(env, {"trigger": {"template": "trigger"},
                   "count": {"language": "cpp", "_outputs": NUM, "_nix": CPP_NIX}},
             [card("begin", "trigger"), card("count", "count")], [])
    put(env.project / "definitions" / "count" / "src" / "card.cc", COUNT)
    from test_cards_container import reload
    reload(c)
    first, st = run(c, "begin", timeout=2400)
    assert st.state == RunState.succeeded, log(c, first, "count", 2)
    second, st = run(c, "begin")
    assert st.state == RunState.succeeded, log(c, second, "count", 2)
    a, b = display(c, first, "count"), display(c, second, "count")
    pa, pb = re.search(r"worker (\d+)", a).group(1), re.search(r"worker (\d+)", b).group(1)
    # The same process; onRunStart started the second run (calls back to 1).
    assert pa == pb and a.endswith("runs 1 calls 1") and b.endswith("runs 2 calls 1"), (a, b)
    assert not invocations(c, first, "count")[0].workerReused
    assert invocations(c, second, "count")[0].workerReused


def test_file_inputs_keep_the_worker_and_stay_read_only(env):
    # The bash run writes its File output through the passed folder
    # descriptor; nothing else of the call's descriptors is there.
    make = bash('test -d /proc/self/fd/900 && test ! -e /proc/self/fd/901 && '
                'echo "line $TSUGUMI_INVOCATION-$RANDOM" > "$TSUGUMI_OUTPUT_DIR/a.txt" && '
                'printf \'{"doc":{"value":"%s"}}\' "$TSUGUMI_OUTPUT_DIR/a.txt" > "$TSUGUMI_OUTPUT"', outputs=DOC)
    c = load(env, {"trigger": {"template": "trigger"}, "make": make,
                   "read": {"language": "cpp", "inputs": [{"name": "doc"}], "_outputs": NUM, "_nix": CPP_NIX}},
             [card("begin", "trigger"), card("make", "make"), card("read", "read")],
             [conn("make", "doc", "read", "doc")])
    put(env.project / "definitions" / "read" / "src" / "card.cc", READ)
    from test_cards_container import reload
    reload(c)
    first, st = run(c, "begin", timeout=2400)
    detail = {k: c.call("RunRecordRequest", runId=first, card=path(k)).status for k in ("make", "read")}
    assert st.state == RunState.succeeded, ({k: v.failure and text(v.failure.diagnostic) for k, v in detail.items()},
                                            log(c, first, "make", 2), log(c, first, "read", 2))
    second, st = run(c, "begin")
    assert st.state == RunState.succeeded, log(c, second, "read", 2)
    # Another input path (another run's output) and the same worker: the stream goes on.
    assert display(c, second, "read") == "2 lines"
    assert invocations(c, second, "read")[0].workerReused
    # The output path recorded is the real one, inside the run's tree.
    rec = c.call("RunRecordRequest", runId=second, card=path("make"))
    assert rec.status.state == CardState.succeeded


def test_fresh_worker_per_call(env):
    nix = CPP_NIX.replace("src = ./src; };\n", "src = ./src; };\n  reuseWorker = false;\n")
    out = ("namespace t; table Num { value:ulong; }\ntable Outputs { again:Num; }\nroot_type Outputs;\n")
    seed = bash('echo \'{"out":{"value":3}}\' > "$TSUGUMI_OUTPUT"')
    c = load(env, {"trigger": {"template": "trigger"}, "seed": seed,
                   "loop": {"template": "merge", "inputs": [{"name": "init"}, {"name": "again"}], "maxRuns": 10},
                   "fresh": {"language": "cpp", "inputs": [{"name": "x"}], "maxRuns": 10, "_outputs": out, "_nix": nix}},
             [card("begin", "trigger"), card("seed", "seed"), card("loop", "loop"), card("fresh", "fresh")],
             [conn("seed", "out", "loop", "init"), conn("loop", "out", "fresh", "x"), conn("fresh", "again", "loop", "again")])
    put(env.project / "definitions" / "fresh" / "src" / "card.cc", FRESH)
    from test_cards_container import reload
    reload(c)
    run_id, st = run(c, "begin", timeout=2400)
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
    loop_nix = "{ pkgs, inputs, params }:\nlet f = x: f (x + 1); in { run = f 0; }\n"
    slow_nix = ("{ pkgs, inputs, params }:\n{ run = pkgs.runCommand \"tsugumi-test-slow-${toString builtins.currentTime}\" { } "
                "\"sleep 60; mkdir -p $out/lib\"; }\n")
    ok = bash('echo \'{"out":{"value":1}}\' > "$TSUGUMI_OUTPUT"')
    import time
    c = load(env, {"trigger": {"template": "trigger"},
                   "loop": {"language": "bash", "_outputs": NUM, "_nix": loop_nix},
                   "slow": {"language": "bash", "buildTimeoutSeconds": 3, "_outputs": NUM, "_nix": slow_nix}, "ok": ok},
             [card("begin", "trigger"), card("loop", "loop"), card("slow", "slow"), card("ok", "ok")], [])
    t0 = time.time()
    run_id, st = run(c, "begin", timeout=600)
    took = time.time() - t0
    rec = {k: c.call("RunRecordRequest", runId=run_id, card=path(k)).status for k in ("loop", "slow", "ok")}
    assert rec["loop"].state == CardState.failed
    diag = text(rec["loop"].failure.diagnostic)
    assert "recursion" in diag or "max-call-depth" in diag or "stack overflow" in diag, diag
    assert rec["slow"].state == CardState.failed and rec["slow"].failure.kind == 14, text(rec["slow"].failure.diagnostic)
    assert took < 50, took  # the 60 s build was stopped
    assert rec["ok"].state == CardState.succeeded
    # tsugumid and its evaluator still work.
    c.call("ProjectInspectRequest")
    run_id, st = run(c, "ok")
    assert c.call("RunRecordRequest", runId=run_id, card=path("ok")).status.state == CardState.succeeded
