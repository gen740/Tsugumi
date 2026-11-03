"""Code Card behaviour in the real environment (TSUGUMI_TEST_CONTAINER=1; the
shared VM):
backpressure stalls, the shell step contract, deadlines and cancellation,
worker reuse, isolation, Nix upstream references, stages and caching, and
the display variants of the Python SDK."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import time
from pathlib import Path

import pytest

from conftest import real_environment
from projects import card, conn
from test_regressions import put, write
from tsugumi_client import RpcFailure, new_id, path, text
from Tsugumi.Data.CardState import CardState
from Tsugumi.Data.Phase import Phase
from Tsugumi.Data.RunState import RunState
from Tsugumi.RPC.NotificationPayload import NotificationPayload

pytestmark = pytest.mark.skipif(os.environ.get("TSUGUMI_TEST_CONTAINER") != "1",
                                reason="needs the container environment (TSUGUMI_TEST_CONTAINER=1)")

NUM = "namespace t; table Num { value:ulong; } table Outputs { out:Num (required); } root_type Outputs;"
TXT = "namespace t; table Txt { value:string; } table Outputs { out:Txt (required); } root_type Outputs;"
FLAG = "namespace t; table Flag { value:bool; } table Outputs { flag:Flag (required); } root_type Outputs;"


def nix_string(s: str) -> str:
    """A Nix string literal; `${` stays literal shell text (not antiquotation)."""
    return json.dumps(s).replace("${", "\\${")


def bash(script: str, inputs=(), outputs=NUM, pre=None, post=None, extra="", **settings) -> dict:
    attrs = [f"run = {nix_string(script)};"]
    if pre:
        attrs.append(f"preRun = {nix_string(pre)};")
    if post:
        attrs.append(f"postRun = {nix_string(post)};")
    body = {"language": "bash", "inputs": [{"name": i} for i in inputs], **settings}
    body["_outputs"] = outputs
    body["_nix"] = "{ pkgs, inputs, params }:\n{\n  " + "\n  ".join(attrs) + "\n" + extra + "}\n"
    return body


def make(root: Path, defs: dict, cards, conns) -> None:
    plain = {}
    for d, body in defs.items():
        body = dict(body)
        nix = body.pop("_nix", None)
        plain[d] = body
        if nix:
            put(root / "definitions" / d / "default.nix", nix)
    write(root, plain, cards, conns)


@pytest.fixture(scope="module")
def env(tmp_path_factory):
    """One container for the module; each test swaps in its own project files."""
    root = tmp_path_factory.mktemp("cards") / "p"
    write(root, {"trigger": {"template": "trigger"}}, [card("start", "trigger")], [])
    e = real_environment(root, os.environ.get("TSUGUMI_TEST_CARDS_NAME", "tsugumi-test-cards"))
    yield e
    e.cleanup()


def reload(c):
    """Reload; the VM's shared folder may need a moment to show new files."""
    for attempt in range(20):
        snap = c.call("ProjectInspectRequest")
        try:
            return c.call("ProjectReloadRequest", baseRev=snap.rev, opId=new_id(), origin="test")
        except RpcFailure as e:
            if e.kind != "invalid_document" or attempt == 19:
                raise
            time.sleep(0.5)


def load(env, defs, cards, conns):
    """Replaces the project's documents and takes them in with Reload."""
    root = env.project
    shutil.rmtree(root / "definitions", ignore_errors=True)
    make(root, defs, cards, conns)
    c = env.client()
    reload(c)
    return c


def run(c, card_id, propagate=True, timeout=900):
    snap = c.call("ProjectInspectRequest")
    acc = c.call("RunRequest", baseRev=snap.rev, opId=new_id(), origin="t", card=path(*card_id.split("/")),
                 propagate=propagate)
    run_id = text(acc.run.runId)
    deadline = time.time() + timeout
    while time.time() < deadline:
        st = c.call("RunStatusRequest", runId=run_id)
        if st.state in (RunState.succeeded, RunState.failed, RunState.cancelled):
            return run_id, st
        time.sleep(0.2)
    raise TimeoutError(run_id)


def log(c, run_id, card_id, stream=1, inv=1):
    return text(c.call("LogRangeRequest", runId=run_id, card=path(*card_id.split("/")), invocation=inv,
                       stream=stream, limitBytes=262144).text)


def test_backpressure_stall_ends_the_run(env):
    """loop(any) -> step -> loop (feedback); step also feeds wait.value, whose
    condition never arrives: its queue fills, everything stops, the run ends."""
    defs = {
        "trigger": {"template": "trigger"},
        "seed": bash('echo \'{"out":{"value":1}}\' > "$TSUGUMI_OUTPUT"'),
        "loop": {"template": "merge", "inputs": [{"name": "init"}, {"name": "again"}], "maxRuns": 1000},
        "step": bash('echo \'{"out":{"value":1}}\' > "$TSUGUMI_OUTPUT"', inputs=["x"], maxRuns=1000,
                     reuseWorker=True),
        "flag": {"template": "input", "params": [{"name": "value", "type": "bool", "data": False}], "_outputs": FLAG},
        "branch": {"template": "branch"},
    }
    c = load(env, defs,
             [card("start", "trigger"), card("seed", "seed"), card("loop", "loop"), card("step", "step"),
              card("never", "flag"), card("gate", "branch"), card("wait", "branch")],
             [conn("seed", "out", "loop", "init"), conn("loop", "out", "step", "x"), conn("step", "out", "loop", "again"),
              conn("step", "out", "wait", "value"), conn("never", "flag", "gate", "value"),
              conn("never", "flag", "gate", "condition"), conn("gate", "a", "wait", "condition")])
    run_id, st = run(c, "start", timeout=600)
    assert st.state == RunState.failed
    assert "cannot progress" in text(st.failure.diagnostic)


def test_shell_step_contract(env):
    big = "x" * 300_000
    defs = {
        "trigger": {"template": "trigger"},
        "make": bash(f'printf \'{{"out":{{"value":"{big}"}}}}\' > "$TSUGUMI_OUTPUT"', outputs=TXT),
        "use": bash(
            # preRun's cd and exports carry over; the input is a JSON file only (no .bin, no INPUT_).
            'test "$(basename "$PWD")" = sub && test "$FROM_PRE" = yes && test -f "$JSON_IN" && '
            'test "$(wc -c < "$JSON_IN")" -gt 300000 && test -z "${INPUT_IN+x}" && test -z "${BIN_IN+x}" && '
            'test -z "$(ls "$TSUGUMI_TMP_DIR" | grep -v -e "^input-in.json$" -e "^pre-env$")" && '
            'echo \'{"out":{"value":7}}\' > "$TSUGUMI_OUTPUT"',
            # preRun and postRun never see the Card inputs.
            inputs=["in"], pre='test -z "${JSON_IN+x}" && mkdir -p sub && cd sub && export FROM_PRE=yes',
            post='test -z "${JSON_IN+x}" && test "$TSUGUMI_FAILED" = 0 && test "$TSUGUMI_OUTPUTS" = out',
            filesystem=True),
        "small": bash('test "$(jq .value "$JSON_IN")" = 7 && echo \'{"out":{"value":8}}\' > "$TSUGUMI_OUTPUT"', inputs=["in"]),
        "postfail": bash('echo \'{"out":{"value":1}}\' > "$TSUGUMI_OUTPUT"', post="exit 3"),
        "runfail": bash("exit 4", post='test "$TSUGUMI_FAILED" = 1'),
        "slow": bash("sleep 1.5; echo '{\"out\":{\"value\":1}}' > \"$TSUGUMI_OUTPUT\"", pre="sleep 1.5", timeoutSeconds=2),
    }
    c = load(env, defs,
             [card("start", "trigger"), card("make", "make"), card("use", "use"), card("small", "small"),
              card("postfail", "postfail"), card("runfail", "runfail"), card("slow", "slow")],
             [conn("make", "out", "use", "in"), conn("use", "out", "small", "in")])
    run_id, st = run(c, "start")
    states = {k: c.call("RunRecordRequest", runId=run_id, card=path(k)) for k in
              ("make", "use", "small", "postfail", "runfail", "slow")}
    detail = {k: (r.status.state, r.status.failure and text(r.status.failure.diagnostic)) for k, r in states.items()}
    assert states["make"].status.state == CardState.succeeded, (detail, log(c, run_id, "make", 2)[-800:])
    assert states["use"].status.state == CardState.succeeded, (detail, log(c, run_id, "use", 2)[-800:])
    assert states["small"].status.state == CardState.succeeded, log(c, run_id, "small", 2)
    # A later successful step never clears an earlier failure; post failure fails the Card.
    assert states["postfail"].status.state == CardState.failed
    assert states["runfail"].status.state == CardState.failed
    assert "run failed" in text(states["runfail"].status.failure.diagnostic)
    # One deadline for all steps: 1.5 s + 1.5 s exceeds 2 s.
    assert states["slow"].status.state == CardState.failed
    assert states["slow"].status.failure.kind == 14  # timeout
    assert st.state == RunState.failed


def proc_count(env, pattern: str) -> int:
    """Processes in the environment whose command line contains `pattern`
    (NUL separators removed). Bash builtins only: the image has no grep."""
    # The pattern is written with a bracket so this probe never matches itself.
    glob = pattern[:-1] + "[" + pattern[-1] + "]"
    script = ("n=0; for f in /proc/[0-9]*/cmdline; do c=$(< \"$f\") 2>/dev/null; "
              f"case \"$c\" in *{glob}*) n=$((n+1));; esac; done; echo $n")
    out = env.exec(script).strip().splitlines()
    return int(out[-1] if out else "0")


@pytest.mark.parametrize("sandbox", [True, False])
def test_cancel_ends_every_descendant(env, sandbox):
    defs = {"trigger": {"template": "trigger"},
            "hang": bash("(sleep 3141 &) ; sleep 2718 & sleep 2719; wait", sandbox=sandbox)}
    c = load(env, defs, [card("start", "trigger"), card("hang", "hang")], [])
    snap = c.call("ProjectInspectRequest")
    acc = c.call("RunRequest", baseRev=snap.rev, opId=new_id(), origin="t", card=path("start"))
    run_id = text(acc.run.runId)
    deadline = time.time() + 600
    try:
        while proc_count(env, "sleep271") == 0:
            assert time.time() < deadline, "the Card never started"
            assert c.call("RunStatusRequest", runId=run_id).state not in (RunState.failed, RunState.succeeded)
            time.sleep(0.5)
        t0 = time.time()
        c.call("RunCancelRequest", runId=run_id)
        while c.call("RunStatusRequest", runId=run_id).state not in (RunState.cancelled, RunState.failed):
            time.sleep(0.05)
        assert time.time() - t0 < 5
        time.sleep(1)
        assert proc_count(env, "sleep271") == 0 and proc_count(env, "sleep3141") == 0
    finally:
        c.call("RunCancelRequest", runId=run_id)


def test_isolation(env):
    # Bash builtins only: Cards get no coreutils unless their package brings them.
    probe = ("set +e; fail=0; "
             # Only the Card's own work folder exists under /project (empty parents).
             "[ -e /project/tsugumi.json ] && { echo 'project files visible'; fail=1; }; "
             "[ -e /project/definitions ] && { echo 'definitions visible'; fail=1; }; "
             "[ -e /control/server.ready ] && { echo 'control visible'; fail=1; }; "
             "[ -e /persist ] && { echo 'persist visible'; fail=1; }; "
             "(exec 3<>/dev/tcp/1.1.1.1/80) 2>/dev/null && { echo 'network reachable'; fail=1; }; "
             "[ $fail = 0 ] && echo '{\"out\":{\"value\":1}}' > \"$TSUGUMI_OUTPUT\"; exit $fail")
    open_fs = ("set +e; fail=0; "
               "(exec 3<>/dev/tcp/1.1.1.1/80) 2>/dev/null && { echo 'network reachable'; fail=1; }; "
               "(read -r x < /control/server.ready) 2>/dev/null && { echo 'token readable'; fail=1; }; "
               "[ \"$UID\" = 65534 ] || { echo \"uid $UID\"; fail=1; }; "
               "[ $fail = 0 ] && echo '{\"out\":{\"value\":1}}' > \"$TSUGUMI_OUTPUT\"; exit $fail")
    defs = {"trigger": {"template": "trigger"}, "probe": bash(probe), "open": bash(open_fs, sandbox=False)}
    c = load(env, defs, [card("start", "trigger"), card("probe", "probe"), card("open", "open")], [])
    run_id, st = run(c, "start")
    detail = {k: (c.call("RunRecordRequest", runId=run_id, card=path(k)).status.state, log(c, run_id, k, 1), log(c, run_id, k, 2)[-600:])
              for k in ("probe", "open")}
    assert st.state == RunState.succeeded, detail


def test_nix_upstream_references(env):
    a = bash('echo \'{"out":{"value":1}}\' > "$TSUGUMI_OUTPUT"',
             extra='  exports = { greet = name: "hello ${name}"; };\n')
    b_nix = ("{ pkgs, inputs, params }:\n{\n  run = ''\n    test \"${inputs.upstream.\"a\".exports.greet \"b\"}\" = \"hello b\"\n"
             "    test -n \"${inputs.local.a.run}\"\n    echo '{\"out\":{\"value\":2}}' > \"$TSUGUMI_OUTPUT\"\n  '';\n}\n")
    defs = {"trigger": {"template": "trigger"}, "a": a,
            "b": {"language": "bash", "inputs": [{"name": "x"}], "_outputs": NUM, "_nix": b_nix}}
    c = load(env, defs, [card("start", "trigger"), card("a", "a"), card("b", "b")], [conn("a", "out", "b", "x")])
    run_id, st = run(c, "start")
    assert st.state == RunState.succeeded, log(c, run_id, "b", 2)
    # A downstream reference is reported before any build.
    bad = a.copy()
    bad["_nix"] = "{ pkgs, inputs, params }:\n{ run = \"${inputs.upstream.\"b\".run}\"; }\n"
    c = load(env, {**defs, "a": bad}, [card("start", "trigger"), card("a", "a"), card("b", "b")], [conn("a", "out", "b", "x")])
    snap = c.call("ProjectInspectRequest")
    codes = [text(d.code) for r in snap.resolution for d in r.diagnostics]
    assert "nix_reference_not_guaranteed" in codes


def test_stages_and_cache(env):
    defs = {"trigger": {"template": "trigger"}, "one": bash('echo \'{"out":{"value":1}}\' > "$TSUGUMI_OUTPUT"')}
    c = load(env, defs, [card("start", "trigger"), card("one", "one")], [])
    c.open()
    phases = []
    trace = []

    counted = set()

    def collect(n):
        # The client may offer the same notification again: count each seq once.
        if n.seq in counted:
            return n.valueType == NotificationPayload.RunEvent and n.value.valueType == 5
        counted.add(n.seq)
        if n.valueType == NotificationPayload.RunEvent and n.value.valueType == 3:
            inv = n.value.value.invocation
            if [text(s) for s in inv.card.segments] == ["one"]:
                phases.append((inv.phase, inv.buildCached))
                trace.append((inv.invocation, inv.state, inv.phase, inv.buildCached))
        return n.valueType == NotificationPayload.RunEvent and n.value.valueType == 5

    snap = c.call("ProjectInspectRequest")
    c.call("RunRequest", baseRev=snap.rev, opId=new_id(), origin="t", card=path("start"))
    c.wait_notification(collect, timeout=900)
    seen = []
    for p, _ in phases:
        if not seen or seen[-1] != p:
            seen.append(p)
    order = [Phase.prepare, Phase.evaluate, Phase.build, Phase.execute, Phase.validate, Phase.save]
    ranks = [order.index(p) for p in seen if p in order]
    assert ranks == sorted(ranks), trace[:30]  # stages only move forward
    assert Phase.execute in seen and Phase.save in seen, seen
    phases.clear()
    snap = c.call("ProjectInspectRequest")
    c.call("RunRequest", baseRev=snap.rev, opId=new_id(), origin="t", card=path("start"))
    c.wait_notification(collect, timeout=600)
    assert Phase.build not in [p for p, _ in phases]
    assert any(cached for _, cached in phases)


def test_python_card_has_a_home_while_it_loads(env):
    """Libraries that keep settings under the home folder (astropy,
    matplotlib) work while the Card's module is loaded, before any call."""
    main = '''
import os
from pathlib import Path
HOME = Path.home()                  # read when the module is loaded
(HOME / ".config").mkdir(parents=True, exist_ok=True)
(HOME / ".config" / "probe").write_text("ok")

def main(ctx):
    ctx.display_text(f"{HOME} {os.environ.get('XDG_CACHE_HOME', '')}")
'''
    defs = {"trigger": {"template": "trigger"},
            "show": {"language": "python",
                     "_outputs": "namespace t; table Num { value:ulong; } table Outputs { out:Num; } root_type Outputs;",
                     "_nix": "{ pkgs, inputs, params }:\n{ run = inputs.tsugumi.pythonCard { src = ./src; }; }\n"}}
    root = env.project
    c = load(env, defs, [card("start", "trigger"), card("show", "show")], [])
    put(root / "definitions" / "show" / "src" / "main.py", main)
    reload(c)
    run_id, st = run(c, "start", timeout=1800)
    assert st.state == RunState.succeeded, log(c, run_id, "show", 2)
    rec = c.call("RunRecordRequest", runId=run_id, card=path("show"))
    shown = text(rec.record.display.value.text)
    assert shown.split()[0].endswith("/home"), shown


def test_python_displays(env):
    main = '''
import struct, zlib

def png(w, h):
    raw = b"".join(b"\\x00" + bytes([200, 40, 40]) * w for _ in range(h))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    return b"\\x89PNG\\r\\n\\x1a\\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + \\
        chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")

def main(ctx):
    ctx.display_table(["n", "square"], [[str(i), str(i * i)] for i in range(5)])
    ctx.display_image(png(4, 4), "image/png", "red square")
    ctx.display_points([0, 1, 2], [0, 1, 4], [0, 1, 8], [0xff0000, 0x00ff00, 0x0000ff])
    ctx.display_module("histogram", {"bins": [1, 2, 3]})
'''
    defs = {"trigger": {"template": "trigger"},
            "show": {"language": "python",
                     "_outputs": "namespace t; table Num { value:ulong; } table Outputs { out:Num; } root_type Outputs;",
                     "_nix": "{ pkgs, inputs, params }:\n{ run = inputs.tsugumi.pythonCard { src = ./src; }; }\n"}}
    root = env.project
    c = load(env, defs, [card("start", "trigger"), card("show", "show")], [])
    put(root / "definitions" / "show" / "src" / "main.py", main)
    reload(c)
    run_id, st = run(c, "start", timeout=1800)
    assert st.state == RunState.succeeded, log(c, run_id, "show", 2)
    rec = c.call("RunRecordRequest", runId=run_id, card=path("show"))
    assert rec.record.display is not None and rec.record.display.valueType == 7, log(c, run_id, "show", 2)
    assert text(rec.record.display.value.name) == "histogram"


RICH = """
namespace {ns};
enum Color : ubyte {{ red = 0, green = 1, blue = 2 }}
struct Point {{ x:int; y:int; }}
table Circle {{ r:double; }}
table Square {{ side:double; }}
union Shape {{ Circle, Square }}
table Rich {{
  id:ulong; neg:long; ratio:double; name:string; values:[ulong];
  color:Color; point:Point; shape:Shape;
}}
"""

PY_MAKE = '''
import os
import outputs.rich.a.Rich as Rich
import outputs.rich.a.Point as Point
import outputs.rich.a.Circle as Circle
import outputs.rich.a.Shape as Shape
import outputs.doc.a.Doc as Doc

def main(ctx):
    n = 100_000
    b = ctx.builder(1 << 20)
    name = b.CreateString("名前 ✓")
    Circle.Start(b); Circle.AddR(b, 1.5); circle = Circle.End(b)
    Rich.StartValuesVector(b, n)
    for i in reversed(range(n)):
        b.PrependUint64(i * 3)
    values = b.EndVector()
    Rich.Start(b)
    Rich.AddId(b, 18446744073709551615)
    Rich.AddNeg(b, -9223372036854775808)
    Rich.AddRatio(b, 0.1)
    Rich.AddName(b, name)
    Rich.AddValues(b, values)
    Rich.AddColor(b, 2)
    Rich.AddPoint(b, Point.CreatePoint(b, -7, 8))
    Rich.AddShapeType(b, Shape.Shape.Circle)
    Rich.AddShape(b, circle)
    ctx.output("rich", b, Rich.End(b))
    path = os.path.join(ctx.output_directory, "table.csv")
    with open(path, "w") as f:
        f.write("a,b\\n" * 1000)
    b = ctx.builder()
    p = b.CreateString(path)
    Doc.Start(b); Doc.AddValue(b, p)
    ctx.output("doc", b, Doc.End(b))
'''

CPP_CHECK = r'''
#include <tsugumi/sdk/card.hh>
#include <fstream>
#include <string>
#include "_inputs_generated.h"
#include "_outputs_generated.h"

void run(tsugumi::sdk::Context& ctx) {
  const auto* r = ctx.input<inputs::rich::a::Rich>("rich");
  if (r->id() != 18446744073709551615ull) throw tsugumi::sdk::Failure("output_invalid", "id");
  if (r->neg() != INT64_MIN) throw tsugumi::sdk::Failure("output_invalid", "neg");
  if (!r->values() || r->values()->size() != 100000 || r->values()->Get(99999) != 299997)
    throw tsugumi::sdk::Failure("output_invalid", "values");
  if (r->color() != inputs::rich::a::Color::blue) throw tsugumi::sdk::Failure("output_invalid", "color");
  if (!r->point() || r->point()->x() != -7 || r->point()->y() != 8) throw tsugumi::sdk::Failure("output_invalid", "point");
  if (r->shape_type() != inputs::rich::a::Shape::Circle || r->shape_as_Circle()->r() != 1.5)
    throw tsugumi::sdk::Failure("output_invalid", "shape");
  if (r->name()->str() != "名前 ✓") throw tsugumi::sdk::Failure("output_invalid", "name");
  const auto* doc = ctx.input<inputs::doc::a::Doc>("doc");
  std::ifstream f(doc->value()->str());
  std::string all((std::istreambuf_iterator<char>(f)), {});
  if (all.size() != 4000) throw tsugumi::sdk::Failure("output_invalid", "file size " + std::to_string(all.size()));
  // Same structure, another namespace: structurally compatible downstream.
  flatbuffers::FlatBufferBuilder b;
  std::vector<uint64_t> v(r->values()->begin(), r->values()->end());
  auto vv = b.CreateVector(v);
  auto nm = b.CreateString(r->name()->str());
  auto circle = outputs::checked::b::CreateCircle(b, 1.5);
  outputs::checked::b::Point pt(-7, 8);
  outputs::checked::b::RichBuilder rb(b);
  rb.add_id(r->id());
  rb.add_neg(r->neg());
  rb.add_ratio(r->ratio());
  rb.add_name(nm);
  rb.add_values(vv);
  rb.add_color(outputs::checked::b::Color::blue);
  rb.add_point(&pt);
  rb.add_shape_type(outputs::checked::b::Shape::Circle);
  rb.add_shape(circle.Union());
  b.Finish(rb.Finish());
  ctx.output("checked", b);
}
TSUGUMI_CARD(run)
'''


def test_values_cross_python_cpp_and_shell(env):
    py_nix = "{ pkgs, inputs, params }:\n{ run = inputs.tsugumi.pythonCard { src = ./src; }; }\n"
    cpp_nix = "{ pkgs, inputs, params }:\n{ run = inputs.tsugumi.cppCard { src = ./src; }; }\n"
    a_out = RICH.format(ns="a") + "table Doc { value:string; }\ntable Outputs { rich:Rich (required); doc:Doc (required, tsugumi: \"file\"); }\nroot_type Outputs;\nattribute \"tsugumi\";\n"
    b_out = RICH.format(ns="b") + "table Outputs { checked:Rich (required); }\nroot_type Outputs;\n"
    bad = '''
def main(ctx):
    ctx.output_bytes("rich", b"not a flatbuffer")
'''
    missing = '''
def main(ctx):
    pass
'''
    shell = bash('test "$(wc -c < "$JSON_IN")" -gt 500000 && read -r -n 300 head < "$JSON_IN" && '
                 'case "$(cat "$JSON_IN")" in *18446744073709551615*) ;; *) exit 9;; esac && '
                 'echo \'{"out":{"value":1}}\' > "$TSUGUMI_OUTPUT"', inputs=["in"])
    defs = {"trigger": {"template": "trigger"},
            "make": {"language": "python", "_outputs": a_out, "_nix": py_nix},
            "check": {"language": "cpp", "inputs": [{"name": "rich"}, {"name": "doc"}], "_outputs": b_out, "_nix": cpp_nix},
            "shell": shell,
            "bad": {"language": "python", "_outputs": a_out.replace("doc:Doc (required,", "doc:Doc ("), "_nix": py_nix},
            "missing": {"language": "python", "_outputs": a_out, "_nix": py_nix},
            "after": bash('echo \'{"out":{"value":1}}\' > "$TSUGUMI_OUTPUT"', inputs=["in"])}
    root = env.project
    c = load(env, defs,
             [card("start", "trigger"), card("make", "make"), card("check", "check"), card("shell", "shell"),
              card("bad", "bad"), card("missing", "missing"), card("after_bad", "after"), card("after_missing", "after")],
             [conn("make", "rich", "check", "rich"), conn("make", "doc", "check", "doc"), conn("check", "checked", "shell", "in"),
              conn("bad", "rich", "after_bad", "in"), conn("missing", "rich", "after_missing", "in")])
    put(root / "definitions" / "make" / "src" / "main.py", PY_MAKE)
    put(root / "definitions" / "check" / "src" / "card.cc", CPP_CHECK)
    put(root / "definitions" / "bad" / "src" / "main.py", bad)
    put(root / "definitions" / "missing" / "src" / "main.py", missing)
    reload(c)
    run_id, st = run(c, "start", timeout=2400)
    rec = {k: c.call("RunRecordRequest", runId=run_id, card=path(k)).status for k in
           ("make", "check", "shell", "bad", "missing", "after_bad", "after_missing")}
    detail = {k: (v.state, v.failure and text(v.failure.diagnostic)) for k, v in rec.items()}
    assert rec["make"].state == CardState.succeeded, (detail, log(c, run_id, "make", 2)[-800:])
    assert rec["check"].state == CardState.succeeded, (detail, log(c, run_id, "check", 2)[-800:])
    assert rec["shell"].state == CardState.succeeded, (detail, log(c, run_id, "shell", 2)[-800:])
    # Invalid output never flows on; a missing required output fails the Card.
    assert rec["bad"].state == CardState.failed and rec["bad"].failure.kind == 24, detail
    assert rec["missing"].state == CardState.failed and "missing" in text(rec["missing"].failure.diagnostic), detail
    assert rec["after_bad"].state == CardState.blocked and rec["after_missing"].state == CardState.blocked, detail


SDK_CALC = r"""
#include <card.hh>

#include <dlfcn.h>

#include <cstdlib>
#include <fstream>
#include <string>

static int calls = 0;  // reuseWorker: one worker serves every invocation

static std::string runtime_path() {
  Dl_info info{};
  dladdr(reinterpret_cast<void*>(&tsugumi::cancelled), &info);
  return info.dli_fname ? info.dli_fname : "?";
}

auto run(Card::Input& input) -> Card::Output {
  ++calls;
  // preRun ran for this invocation (postRun removes the file again).
  if (!std::ifstream("pre.txt")) throw tsugumi::Failure("output_invalid", "preRun did not run before call " + std::to_string(calls));
  const char* pre = std::getenv("PRE_VALUE");
  if (!pre || std::string(pre) != "5") throw tsugumi::Failure("output_invalid", "PRE_VALUE not carried over");
  uint64_t n = 0;
  if (const auto* v = input.x.value.Asinit()) n = v->value;
  else if (const auto* v = input.x.value.Asagain()) n = v->value;
  tsugumi::log("runtime " + runtime_path());
  tsugumi::visualization::text("n = " + std::to_string(n));
  Card::Output out;
  if (n > 0) {
    out.again.emplace().value = n - 1;
    return out;
  }
  std::string file = tsugumi::context().output_directory + "/calls.txt";
  std::ofstream(file) << calls;
  out.calls.emplace().value = calls;
  out.report.emplace().value = file;
  return out;
}
"""

SDK_OTHER = r"""
#include <card.hh>

#include <dlfcn.h>

auto run(Card::Input& input) -> Card::Output {
  Dl_info info{};
  dladdr(reinterpret_cast<void*>(&tsugumi::cancelled), &info);
  tsugumi::log(std::string("runtime ") + (info.dli_fname ? info.dli_fname : "?"));
  Card::Output out;
  out.out.emplace().value = input.in.value + Card::params().step.value();
  tsugumi::visualization::text("out = " + std::to_string(out.out->value));
  return out;
}
"""


def test_cpp_sdk_card_api(env):
    """`#include <card.hh>` / run(Card::Input&) -> Card::Output: named
    inputs (a merge union), optional and File
    outputs, params, tsugumi::visualization, `run` as a package, preRun
    and postRun around every invocation of a reused worker, and one shared
    runtime library for two SDKs with different contracts."""
    seed = bash('echo \'{"out":{"value":2}}\' > "$TSUGUMI_OUTPUT"')
    calc_out = ("namespace t; table Num { value:ulong; } table Doc { value:string; }\n"
                "table Outputs { again:Num; calls:Num; report:Doc (tsugumi: \"file\"); }\n"
                "root_type Outputs;\nattribute \"tsugumi\";\n")
    calc_nix = """{ pkgs, inputs, params }:
let
  sdk = inputs.tsugumi.sdk { languages = [ "cpp" ]; };
in
{
  run = inputs.tsugumi.cppCard { inherit sdk; src = ./src; };
  preRun = "echo pre > pre.txt && export PRE_VALUE=5";
  postRun = "test \\"$TSUGUMI_FAILED\\" = 0 && rm pre.txt";
}
"""
    other_nix = calc_nix.replace(
        '  preRun = "echo pre > pre.txt && export PRE_VALUE=5";\n', "").replace(
        '  postRun = "test \\"$TSUGUMI_FAILED\\" = 0 && rm pre.txt";\n', "")
    defs = {"trigger": {"template": "trigger"}, "seed": seed,
            "loop": {"template": "merge", "inputs": [{"name": "init"}, {"name": "again"}], "maxRuns": 10},
            "calc": {"language": "cpp", "inputs": [{"name": "x"}], "maxRuns": 10, "reuseWorker": True,
                     "_outputs": calc_out, "_nix": calc_nix},
            "other": {"language": "cpp", "inputs": [{"name": "in"}], "reuseWorker": False,
                      "params": [{"name": "step", "type": "uint64", "data": 10}],
                      "_outputs": NUM, "_nix": other_nix}}
    root = env.project
    c = load(env, defs,
             [card("start", "trigger"), card("seed", "seed"), card("loop", "loop"), card("calc", "calc"),
              card("other", "other")],
             [conn("seed", "out", "loop", "init"), conn("loop", "out", "calc", "x"),
              conn("calc", "again", "loop", "again"), conn("calc", "calls", "other", "in")])
    put(root / "definitions" / "calc" / "src" / "card.cc", SDK_CALC)
    put(root / "definitions" / "other" / "src" / "card.cc", SDK_OTHER)
    reload(c)
    run_id, st = run(c, "start", timeout=2400)
    rec = {k: c.call("RunRecordRequest", runId=run_id, card=path(k)).status for k in ("calc", "other")}
    detail = {k: (v.state, v.failure and text(v.failure.diagnostic)) for k, v in rec.items()}
    assert rec["calc"].state == CardState.succeeded, (detail, log(c, run_id, "calc", 2)[-1200:])
    assert rec["other"].state == CardState.succeeded, (detail, log(c, run_id, "other", 4)[-1500:])
    assert st.state == RunState.succeeded
    # Three invocations (2, 1, 0) on one worker; preRun/postRun around each.
    invs = c.call("RunInvocationsRequest", runId=run_id, card=path("calc")).records
    assert len(invs) == 3
    runtimes = {line.split(" ", 1)[1] for k, inv in (("calc", 3), ("other", 1))
                for line in log(c, run_id, k, 2, inv).splitlines() if line.startswith("runtime ")}
    assert len(runtimes) == 1 and next(iter(runtimes)).endswith("/lib/libtsugumi_card.so"), runtimes
    # calls = 3 from the reused worker, plus the param step 10.
    shown = c.call("RunRecordRequest", runId=run_id, card=path("other")).record.display
    assert shown is not None and shown.valueType == 1 and text(shown.value.text) == "out = 13"


def test_parallel_limit_serial_card_and_unselected_branch(env):
    """Four independent 1 s Cards with max concurrency 2 take about 2 s; a
    branch side that is never selected is never evaluated (its default.nix
    would not even evaluate)."""
    sleepy = bash('sleep 1; echo \'{"out":{"value":1}}\' > "$TSUGUMI_OUTPUT"')
    broken = {"language": "bash", "inputs": [{"name": "in"}], "_outputs": NUM,
              "_nix": "{ pkgs, inputs, params }:\n{ run = builtins.throw \"must never be evaluated\"; }\n"}
    defs = {"trigger": {"template": "trigger"}, "sleepy": sleepy,
            "flag": {"template": "input", "params": [{"name": "value", "type": "bool", "data": False}], "_outputs": FLAG},
            "branch": {"template": "branch"}, "broken": broken}
    c = load(env, defs,
             [card("start", "trigger"), *[card(f"s{i}", "sleepy") for i in range(4)],
              card("cond", "flag"), card("gate", "branch"), card("never", "broken")],
             [conn("cond", "flag", "gate", "value"), conn("cond", "flag", "gate", "condition"),
              conn("gate", "a", "never", "in")])
    snap = c.call("ProjectInspectRequest")
    t0 = time.time()
    acc = c.call("RunRequest", baseRev=snap.rev, opId=new_id(), origin="t", card=path("start"), maxConcurrency=2)
    run_id = text(acc.run.runId)
    while c.call("RunStatusRequest", runId=run_id).state not in (RunState.succeeded, RunState.failed, RunState.cancelled):
        time.sleep(0.1)
    st = c.call("RunStatusRequest", runId=run_id)
    assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)
    spans = []
    for i in range(4):
        inv = c.call("RunInvocationsRequest", runId=run_id, card=path(f"s{i}")).records[0]
        spans.append((inv.startedAt, inv.endedAt))
    # Never more than two at once.
    events = sorted([(s, 1) for s, _ in spans] + [(e, -1) for _, e in spans])
    running = peak = 0
    for _, d in events:
        running += d
        peak = max(peak, running)
    assert peak <= 2
    assert c.call("RunRecordRequest", runId=run_id, card=path("never")).status.state == CardState.skipped
    del t0


def test_worker_crash_then_edit_and_rerun(env):
    crash = {"language": "python", "_outputs": NUM,
             "_nix": "{ pkgs, inputs, params }:\n{ run = inputs.tsugumi.pythonCard { src = ./src; }; }\n"}
    defs = {"trigger": {"template": "trigger"}, "crash": crash}
    root = env.project
    c = load(env, defs, [card("start", "trigger"), card("crash", "crash")], [])
    put(root / "definitions" / "crash" / "src" / "main.py", "import os, signal\n\ndef main(ctx):\n    os.kill(os.getpid(), signal.SIGKILL)\n")
    reload(c)
    run_id, st = run(c, "start", timeout=1800)
    rec = c.call("RunRecordRequest", runId=run_id, card=path("crash")).status
    assert st.state == RunState.failed and rec.state == CardState.failed
    assert rec.failure.kind == 23  # worker_failed
    # After the crash the environment keeps working: fix the source and run again.
    put(root / "definitions" / "crash" / "src" / "main.py",
        "import outputs.out.t.Num as Num\n\ndef main(ctx):\n    b = ctx.builder()\n    Num.Start(b)\n    Num.AddValue(b, 1)\n    ctx.output('out', b, Num.End(b))\n")
    reload(c)
    _, st2 = run(c, "start", timeout=1800)
    assert st2.state == RunState.succeeded, st2.failure and text(st2.failure.diagnostic)
