"""Code Card behaviour in the real environment (TSUGUMI_TEST_CONTAINER=1):
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

from conftest import ContainerEnv
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


def bash(script: str, inputs=(), outputs=NUM, pre=None, post=None, extra="", **settings) -> dict:
    attrs = [f"script = {json.dumps(script)};"]
    if pre:
        attrs.append(f"preScript = {json.dumps(pre)};")
    if post:
        attrs.append(f"postScript = {json.dumps(post)};")
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
    write(root, {"trigger": {"template": "trigger"}}, [card("begin", "trigger")], [])
    e = ContainerEnv(root, "tsugumi-test-cards").start()
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
        "step": bash('echo \'{"out":{"value":1}}\' > "$TSUGUMI_OUTPUT"', inputs=["x"], maxRuns=1000, reuseWorker=True),
        "flag": {"template": "input", "params": [{"name": "value", "type": "bool", "data": False}], "_outputs": FLAG},
        "branch": {"template": "branch"},
    }
    c = load(env, defs,
             [card("begin", "trigger"), card("seed", "seed"), card("loop", "loop"), card("step", "step"),
              card("never", "flag"), card("gate", "branch"), card("wait", "branch")],
             [conn("seed", "out", "loop", "init"), conn("loop", "out", "step", "x"), conn("step", "out", "loop", "again"),
              conn("step", "out", "wait", "value"), conn("never", "flag", "gate", "value"),
              conn("never", "flag", "gate", "condition"), conn("gate", "a", "wait", "condition")])
    run_id, st = run(c, "begin", timeout=600)
    assert st.state == RunState.failed
    assert "cannot progress" in text(st.failure.diagnostic)


def test_shell_step_contract(env):
    big = "x" * 300_000
    defs = {
        "trigger": {"template": "trigger"},
        "make": bash(f'printf \'{{"out":{{"value":"{big}"}}}}\' > "$TSUGUMI_OUTPUT"', outputs=TXT),
        "use": bash(
            # preScript's cd and exports carry over; JSON_ is a file; INPUT_ is absent for big values.
            'test "$(basename "$PWD")" = sub && test "$FROM_PRE" = yes && test -f "$JSON_IN" && '
            'test "$(wc -c < "$JSON_IN")" -gt 300000 && test -z "${INPUT_IN+x}" && test -f "$BIN_IN" && '
            'echo \'{"out":{"value":7}}\' > "$TSUGUMI_OUTPUT"',
            inputs=["in"], pre='mkdir -p sub && cd sub && export FROM_PRE=yes', filesystem=True),
        "small": bash('test "$INPUT_IN" = 7 && echo \'{"out":{"value":8}}\' > "$TSUGUMI_OUTPUT"', inputs=["in"]),
        "postfail": bash('echo \'{"out":{"value":1}}\' > "$TSUGUMI_OUTPUT"', post="exit 3"),
        "scriptfail": bash("exit 4", post='test "$TSUGUMI_FAILED" = 1'),
        "slow": bash("sleep 1.5; echo '{\"out\":{\"value\":1}}' > \"$TSUGUMI_OUTPUT\"", pre="sleep 1.5", timeoutSeconds=2),
    }
    c = load(env, defs,
             [card("begin", "trigger"), card("make", "make"), card("use", "use"), card("small", "small"),
              card("postfail", "postfail"), card("scriptfail", "scriptfail"), card("slow", "slow")],
             [conn("make", "out", "use", "in"), conn("use", "out", "small", "in")])
    run_id, st = run(c, "begin")
    states = {k: c.call("RunRecordRequest", runId=run_id, card=path(k)) for k in
              ("make", "use", "small", "postfail", "scriptfail", "slow")}
    assert states["use"].status.state == CardState.succeeded, log(c, run_id, "use", 2)
    assert states["small"].status.state == CardState.succeeded, log(c, run_id, "small", 2)
    # A later successful step never clears an earlier failure; post failure fails the Card.
    assert states["postfail"].status.state == CardState.failed
    assert states["scriptfail"].status.state == CardState.failed
    assert "script failed" in text(states["scriptfail"].status.failure.diagnostic)
    # One deadline for all steps: 1.5 s + 1.5 s exceeds 2 s.
    assert states["slow"].status.state == CardState.failed
    assert states["slow"].status.failure.kind == 14  # timeout
    assert st.state == RunState.failed


def proc_count(env, pattern: str) -> int:
    out = subprocess.run(["container", "exec", env.name, "sh", "-c",
                          f"cat /proc/[0-9]*/cmdline 2>/dev/null | tr '\\0' ' ' | grep -o '{pattern}' | wc -l"],
                         capture_output=True, text=True)
    return int(out.stdout.strip() or "0")


@pytest.mark.parametrize("sandbox", [True, False])
def test_cancel_ends_every_descendant(env, sandbox):
    defs = {"trigger": {"template": "trigger"},
            "hang": bash("(sleep 3141 &) ; sleep 2718 & sleep 2719; wait", sandbox=sandbox)}
    c = load(env, defs, [card("begin", "trigger"), card("hang", "hang")], [])
    snap = c.call("ProjectInspectRequest")
    acc = c.call("RunRequest", baseRev=snap.rev, opId=new_id(), origin="t", card=path("begin"))
    run_id = text(acc.run.runId)
    deadline = time.time() + 600
    while proc_count(env, "sleep 271") == 0:
        assert time.time() < deadline, "the Card never started"
        time.sleep(0.5)
    t0 = time.time()
    c.call("RunCancelRequest", runId=run_id)
    while c.call("RunStatusRequest", runId=run_id).state not in (RunState.cancelled, RunState.failed):
        time.sleep(0.05)
    assert time.time() - t0 < 5
    time.sleep(1)
    assert proc_count(env, "sleep 271") == 0 and proc_count(env, "sleep 3141") == 0


def test_isolation(env):
    probe = ("! ls /project 2>/dev/null && ! cat /control/server.ready 2>/dev/null && "
             "! (exec 3<>/dev/tcp/1.1.1.1/80) 2>/dev/null && ! ls /persist 2>/dev/null && "
             "echo '{\"out\":{\"value\":1}}' > \"$TSUGUMI_OUTPUT\"")
    open_fs = ("! (exec 3<>/dev/tcp/1.1.1.1/80) 2>/dev/null && ! cat /control/server.ready 2>/dev/null && "
               "test \"$(id -u)\" = 65534 && echo '{\"out\":{\"value\":1}}' > \"$TSUGUMI_OUTPUT\"")
    defs = {"trigger": {"template": "trigger"}, "probe": bash(probe), "open": bash(open_fs, sandbox=False)}
    c = load(env, defs, [card("begin", "trigger"), card("probe", "probe"), card("open", "open")], [])
    run_id, st = run(c, "begin")
    assert st.state == RunState.succeeded, (log(c, run_id, "probe", 2), log(c, run_id, "open", 2))


def test_nix_upstream_references(env):
    a = bash('echo \'{"out":{"value":1}}\' > "$TSUGUMI_OUTPUT"',
             extra='  exports = { greet = name: "hello ${name}"; };\n')
    b_nix = ("{ pkgs, inputs, params }:\n{\n  script = ''\n    test \"${inputs.upstream.\"a\".exports.greet \"b\"}\" = \"hello b\"\n"
             "    test -n \"${inputs.local.a.script}\"\n    echo '{\"out\":{\"value\":2}}' > \"$TSUGUMI_OUTPUT\"\n  '';\n}\n")
    defs = {"trigger": {"template": "trigger"}, "a": a,
            "b": {"language": "bash", "inputs": [{"name": "x"}], "_outputs": NUM, "_nix": b_nix}}
    c = load(env, defs, [card("begin", "trigger"), card("a", "a"), card("b", "b")], [conn("a", "out", "b", "x")])
    run_id, st = run(c, "begin")
    assert st.state == RunState.succeeded, log(c, run_id, "b", 2)
    # A downstream reference is reported before any build.
    bad = a.copy()
    bad["_nix"] = "{ pkgs, inputs, params }:\n{ script = \"${inputs.upstream.\"b\".script}\"; }\n"
    c = load(env, {**defs, "a": bad}, [card("begin", "trigger"), card("a", "a"), card("b", "b")], [conn("a", "out", "b", "x")])
    snap = c.call("ProjectInspectRequest")
    codes = [text(d.code) for r in snap.resolution for d in r.diagnostics]
    assert "nix_reference_not_guaranteed" in codes


def test_stages_and_cache(env):
    defs = {"trigger": {"template": "trigger"}, "one": bash('echo \'{"out":{"value":1}}\' > "$TSUGUMI_OUTPUT"')}
    c = load(env, defs, [card("begin", "trigger"), card("one", "one")], [])
    c.open()
    phases = []

    def collect(n):
        if n.valueType == NotificationPayload.RunEvent and n.value.valueType == 3:
            inv = n.value.value.invocation
            if [text(s) for s in inv.card.segments] == ["one"]:
                phases.append((inv.phase, inv.buildCached))
        return n.valueType == NotificationPayload.RunEvent and n.value.valueType == 5

    snap = c.call("ProjectInspectRequest")
    c.call("RunRequest", baseRev=snap.rev, opId=new_id(), origin="t", card=path("begin"))
    c.wait_notification(collect, timeout=900)
    seen = [p for p, _ in phases]
    order = [Phase.prepare, Phase.evaluate, Phase.build, Phase.execute, Phase.save]
    assert [p for p in seen if p in order] == [p for p in order if p in seen]
    phases.clear()
    snap = c.call("ProjectInspectRequest")
    c.call("RunRequest", baseRev=snap.rev, opId=new_id(), origin="t", card=path("begin"))
    c.wait_notification(collect, timeout=600)
    assert Phase.build not in [p for p, _ in phases]
    assert any(cached for _, cached in phases)


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
            "show": {"language": "python", "canvasEnabled": True,
                     "_outputs": "namespace t; table Num { value:ulong; } table Outputs { out:Num; } root_type Outputs;",
                     "_nix": "{ pkgs, inputs, params }:\n{ package = inputs.tsugumi.pythonCard { src = ./src; }; }\n"}}
    root = env.project
    c = load(env, defs, [card("begin", "trigger"), card("show", "show")], [])
    put(root / "definitions" / "show" / "src" / "main.py", main)
    reload(c)
    run_id, st = run(c, "begin", timeout=1800)
    assert st.state == RunState.succeeded, log(c, run_id, "show", 2)
    rec = c.call("RunRecordRequest", runId=run_id, card=path("show"))
    assert rec.record.display is not None and rec.record.display.valueType == 7, log(c, run_id, "show", 2)
    assert text(rec.record.display.value.name) == "histogram"
