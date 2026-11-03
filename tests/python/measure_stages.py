"""Where the time of a run goes: per stage of every invocation (SDK prepare,
Nix evaluate and build, execute, validate, save), the wait for a free lane
after the inputs arrived, and how long a Card's completion takes to reach a
client. A first run and a rerun are measured separately.

    python measure_stages.py collatz            # demo, in the shared VM
    python measure_stages.py builtin-chain      # builtin Cards only, host tsugumid
    python measure_stages.py large-values       # 100 000-element values, Python -> C++ -> shell (shared VM)

Prints a table per run and writes JSON next to it (--out)."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import statistics
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(__file__))

from conftest import ROOT, Daemon, real_environment  # noqa: E402
from projects import card, conn  # noqa: E402
from test_regressions import write, put  # noqa: E402
from tsugumi_client import new_id, path, text  # noqa: E402
from Tsugumi.Data.Phase import Phase  # noqa: E402
from Tsugumi.RPC.NotificationPayload import NotificationPayload  # noqa: E402

PHASES = {v: k for k, v in vars(Phase).items() if not k.startswith("_")}


def builtin_chain(root: Path, n: int = 40) -> str:
    """trigger → input → n × (fork → merge) → logger: many short builtin invocations."""
    fbs = "namespace m; table Msg { text:string; } table Outputs { msg:Msg (required); } root_type Outputs;"
    defs = {"trigger": {"template": "trigger"},
            "msg": {"template": "input", "params": [{"name": "text", "type": "string", "data": "x"}], "_outputs": fbs},
            "fork": {"template": "fork"},
            "merge": {"template": "merge", "inputs": [{"name": "a"}, {"name": "b"}], "maxRuns": 4},
            "log": {"template": "logger"}}
    cards = [card("start", "trigger"), card("src", "msg")]
    conns = [conn("start", "out", "src", "start")]
    prev = ("src", "msg")
    for i in range(n):
        cards += [card(f"f{i}", "fork"), card(f"m{i}", "merge")]
        conns += [conn(prev[0], prev[1], f"f{i}", "in"), conn(f"f{i}", "out1", f"m{i}", "a")]
        prev = (f"m{i}", "out")
    cards.append(card("out", "log"))
    conns.append(conn(prev[0], prev[1], "out", "in"))
    write(root, defs, cards, conns)
    return "start"


def all_cards(c, scope=()):
    snap = c.call("ProjectInspectRequest")
    out = []
    for r in snap.resolution:
        out.append(tuple(text(s) for s in r.card.segments))
    return out


def run_once(c, start: str, timeout: float):
    received = {}
    kinds = {}

    def hook(n, t):
        kinds[(n.valueType, getattr(n.value, "valueType", None))] = kinds.get((n.valueType, getattr(n.value, "valueType", None)), 0) + 1
        if n.valueType == NotificationPayload.RunEvent and n.value.valueType == 3:
            inv = n.value.value.invocation
            if inv.endedAt:
                received[(tuple(text(s) for s in inv.card.segments), inv.invocation)] = t
    c.on_notification = hook
    snap = c.call("ProjectInspectRequest")
    t0 = time.time()
    acc = c.call("RunRequest", baseRev=snap.rev, opId=new_id(), origin="measure", card=path(*start.split("/")))
    run_id = text(acc.run.runId)
    deadline = time.time() + timeout
    while time.time() < deadline:
        st = c.call("RunStatusRequest", runId=run_id)
        if st.state in (6, 7, 8):
            break
        time.sleep(0.1)
    wall = time.time() - t0
    c.on_notification = None
    if os.environ.get("MEASURE_DEBUG"):
        print("notifications", kinds, "kept", len(received))
    if st.state != 6 and st.failure:
        print("run failed:", text(st.failure.diagnostic))
    stage_ms: dict[str, list[int]] = {}
    waits, notify, durations = [], [], []
    count = 0
    for card_path in all_cards(c):
        records, cursor = [], None
        try:
            while True:
                fields = {"cursor": cursor} if cursor else {}
                page = c.call("RunInvocationsRequest", runId=run_id, card=path(*card_path), limit=500, **fields)
                records += page.records
                cursor = text(page.nextCursor) if page.nextCursor else None
                if not cursor:
                    break
        except Exception:
            continue  # never ran in this run
        for r in records:
            count += 1
            if r.durationMs is not None:
                durations.append(r.durationMs)
            for s in r.stages or []:
                if s.end is not None and s.start is not None:
                    stage_ms.setdefault(f"{PHASES.get(s.phase, s.phase)}/{s.outcome}", []).append(s.end - s.start)
            if r.readyAt and r.startedAt:
                waits.append(r.startedAt - r.readyAt)
            got = received.get((card_path, r.invocation))
            if got and r.endedAt:
                notify.append(got * 1000 - r.endedAt)
    return {"run_id": run_id, "state": st.state, "wall_s": round(wall, 3), "invocations": count,
            "stages": {k: summarize(v) for k, v in sorted(stage_ms.items())},
            "queue_wait_ms": summarize(waits), "notify_ms": summarize(notify),
            "invocation_ms": summarize(durations)}


def summarize(v):
    if not v:
        return None
    v = sorted(v)
    return {"n": len(v), "total": round(sum(v), 1), "median": round(statistics.median(v), 1),
            "p95": round(v[min(len(v) - 1, int(len(v) * 0.95))], 1), "max": round(v[-1], 1)}


OUTCOME = {1: "running", 2: "done", 3: "cached", 4: "reused", 5: "failed", 6: "cancelled"}


def show(label, r):
    print(f"\n== {label}: run {r['run_id'][:8]} state={r['state']} wall={r['wall_s']} s invocations={r['invocations']}")
    print(f"{'stage/outcome':<22}{'n':>6}{'total ms':>12}{'median':>10}{'p95':>10}{'max':>10}")
    for k, s in r["stages"].items():
        phase, outcome = k.split("/")
        name = f"{phase}/{OUTCOME.get(int(outcome), outcome)}"
        print(f"{name:<22}{s['n']:>6}{s['total']:>12}{s['median']:>10}{s['p95']:>10}{s['max']:>10}")
    for key, name in (("queue_wait_ms", "queue wait"), ("notify_ms", "notify latency"), ("invocation_ms", "invocation")):
        s = r[key]
        if s:
            print(f"{name:<22}{s['n']:>6}{s['total']:>12}{s['median']:>10}{s['p95']:>10}{s['max']:>10}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("scenario", choices=["collatz", "builtin-chain", "large-values"])
    ap.add_argument("--out", default=None)
    ap.add_argument("--name", default="tsugumi-measure")
    ap.add_argument("--host", action="store_true", help="collatz on a host tsugumid (no VM), for comparison")
    a = ap.parse_args()
    tmp = Path(tempfile.mkdtemp(prefix="tsgm-"))
    root = tmp / "p"
    env = None
    try:
        if a.scenario == "builtin-chain":
            start = builtin_chain(root)
            d = Daemon(root).start()
            env = d
            c = d.client()
            c.hello()
            c.open()  # notifications go to opened views
        else:
            if a.scenario == "collatz":
                shutil.copytree(ROOT / "demos" / "collatz", root)
                start = "start"
            else:
                import test_cards_container as T
                start = "start"
                T.make(root, {"trigger": {"template": "trigger"}}, [card("start", "trigger")], [])
            if a.host:
                # The host has no bubblewrap: Cards run unsandboxed, as in test_demos.
                for d in (root / "definitions").iterdir():
                    cj = json.loads((d / "_card.json").read_text())
                    if cj.get("language"):
                        cj["sandbox"] = False
                        (d / "_card.json").write_text(json.dumps(cj))
                env = Daemon(root).start()
                c = env.client()
                c.hello()
            else:
                env = real_environment(root, a.name)
                c = env.client()
            if a.scenario == "large-values":
                import test_cards_container as T
                py_nix = "{ pkgs, inputs, params }:\n{ run = inputs.tsugumi.pythonCard { src = ./src; }; }\n"
                cpp_nix = "{ pkgs, inputs, params }:\n{ run = inputs.tsugumi.cppCard { src = ./src; }; }\n"
                a_out = T.RICH.format(ns="a") + "table Doc { value:string; }\ntable Outputs { rich:Rich (required); doc:Doc (required, tsugumi: \"file\"); }\nroot_type Outputs;\nattribute \"tsugumi\";\n"
                b_out = T.RICH.format(ns="b") + "table Outputs { checked:Rich (required); }\nroot_type Outputs;\n"
                shell = T.bash('test "$(wc -c < "$JSON_IN")" -gt 500000 && echo \'{"out":{"value":1}}\' > "$TSUGUMI_OUTPUT"', inputs=["in"])
                c = T.load(env, {"trigger": {"template": "trigger"},
                                 "make": {"language": "python", "_outputs": a_out, "_nix": py_nix},
                                 "check": {"language": "cpp", "inputs": [{"name": "rich"}, {"name": "doc"}], "_outputs": b_out, "_nix": cpp_nix},
                                 "shell": shell},
                           [card("start", "trigger"), card("make", "make"), card("check", "check"), card("shell", "shell")],
                           [conn("make", "rich", "check", "rich"), conn("make", "doc", "check", "doc"), conn("check", "checked", "shell", "in")])
                put(root / "definitions" / "make" / "src" / "main.py", T.PY_MAKE)
                put(root / "definitions" / "check" / "src" / "card.cc", T.CPP_CHECK)
                T.reload(c)
        if a.scenario != "builtin-chain":
            c.open()
        first = run_once(c, start, 3600)
        show("first run", first)
        second = run_once(c, start, 3600)
        show("rerun", second)
        if a.out:
            Path(a.out).write_text(json.dumps({"scenario": a.scenario, "first": first, "rerun": second}, indent=1))
    finally:
        if env is not None:
            env.cleanup()
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
