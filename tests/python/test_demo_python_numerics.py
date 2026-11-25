"""Check the maximum flow and minimum cut against independent implementations."""

from __future__ import annotations

# TODO(Card ABI): the bundled demos are still on the previous Card ABI and are ported once the
# recording of displays is designed (docs/Demos.md, "TODO: port the demos to
# the current Card ABI"). Until then these demo runs are skipped, not ported.
import pytest

pytest.skip("TODO(Card ABI): demos are postponed until display recording is designed; "
            "see docs/Demos.md", allow_module_level=True)

import itertools
import json
import math
import os
from collections import deque
from pathlib import Path

import pytest

from conftest import demo_environment
from projects import start_run, wait_run
from test_demos import connect, edit_card, set_param, stderr
from tsugumi_client import path, text
from Tsugumi.Data.DisplayValue import DisplayValue
from Tsugumi.Data.RunState import RunState

pytestmark = pytest.mark.skipif(os.environ.get("TSUGUMI_TEST_DEMO_RUNS") != "1",
                                reason="runs the demo (large Nix closures): TSUGUMI_TEST_DEMO_RUNS=1")


def stdout(c, run_id, *card):
    return text(c.call("LogRangeRequest", runId=run_id, card=path(*card), stream=1, limitBytes=262144).text)


def result(c, run_id, card):
    """The `result: ...` line a Card printed."""
    line = next(l for l in stdout(c, run_id, card).splitlines() if l.startswith("result:"))
    return json.loads(line[len("result:"):])


def run_ok(c, card, cards, propagate=True):
    rid = start_run(c, [card], propagate=propagate)
    st = wait_run(c, rid)
    assert st.state == RunState.succeeded, (st.failure and text(st.failure.diagnostic), {x: stderr(c, rid, x)[-1500:] for x in cards})
    return rid


def save_figures(c, rid, demo, cards, suffix=""):
    """Saves the displays when TSUGUMI_DEMO_FIGURES names a folder, and returns nothing
    else: the figures are for a person to look at. Also reports each Card's time."""
    folder = os.environ.get("TSUGUMI_DEMO_FIGURES")
    if not folder:
        return
    root = Path(folder) / demo
    root.mkdir(parents=True, exist_ok=True)
    times = {}
    for card in cards:
        rec = c.call("RunRecordRequest", runId=rid, card=path(card)).record
        times[card] = rec.summary.durationMs, [(s.phase, s.end - s.start) for s in rec.summary.stages or []]
        d = rec.display if rec else None
        if not d:
            continue
        value = d.value
        if d.valueType == 5:
            (root / f"{card}{suffix}.png").write_bytes(bytes(value.data))
        elif d.valueType == 7:
            (root / f"{card}{suffix}.json").write_text(text(value.configJson))
        elif hasattr(value, "text"):
            (root / f"{card}{suffix}.md").write_text(text(value.text))
    (root / f"times{suffix}.json").write_text(json.dumps(times))


def assert_no_missing_glyphs(c, rid, cards):
    for card in cards:
        log = stderr(c, rid, card)
        assert "missing from" not in log and "does not have a glyph" not in log, (card, log[-500:])  # plain text, mathtext


FLOW = ["network", "flow", "cut", "verify", "whatif", "sweep"]


def edmonds_karp(nodes, edges, s="s", t="t"):
    residual = {u: {} for u in nodes}
    for u, v, cap in edges:
        residual[u][v] = residual[u].get(v, 0) + cap
        residual[v].setdefault(u, 0)
    total = 0
    while True:
        parent = {s: None}
        queue = deque([s])
        while queue and t not in parent:
            u = queue.popleft()
            for v, cap in residual[u].items():
                if cap > 0 and v not in parent:
                    parent[v] = u
                    queue.append(v)
        if t not in parent:
            return total
        bottleneck = math.inf
        v = t
        while parent[v] is not None:
            bottleneck = min(bottleneck, residual[parent[v]][v])
            v = parent[v]
        v = t
        while parent[v] is not None:
            residual[parent[v]][v] -= bottleneck
            residual[v][parent[v]] += bottleneck
            v = parent[v]
        total += bottleneck


def brute_cuts(nodes, edges):
    """(The minimum capacity, how many s sides reach it.)"""
    inner = [n for n in nodes if n not in ("s", "t")]
    best = math.inf
    count = 0
    for r in range(len(inner) + 1):
        for chosen in itertools.combinations(inner, r):
            side = {"s", *chosen}
            cap = sum(c for u, v, c in edges if u in side and v not in side)
            if cap < best:
                best, count = cap, 1
            elif cap == best:
                count += 1
    return best, count


def test_max_flow(tmp_path):
    env = demo_environment(tmp_path, "max-flow")
    try:
        c = connect(env)
        rid = run_ok(c, "start", FLOW)
        save_figures(c, rid, "max-flow", FLOW)
        assert_no_missing_glyphs(c, rid, ["flow", "cut", "sweep"])

        net = result(c, rid, "network")
        nodes, edges = net["nodes"], [tuple(e) for e in net["edges"]]
        reference = edmonds_karp(nodes, edges)
        best, count = brute_cuts(nodes, edges)
        assert reference == best

        flow = result(c, rid, "flow")
        cut = result(c, rid, "cut")
        verify = result(c, rid, "verify")
        assert flow["value"] == cut["capacity"] == reference
        assert verify["networkx"] == verify["scipy"] == reference
        assert len(verify["minimum_cuts"]) == count
        assert sum(e[2] for e in cut["cut_edges"]) == reference

        # Every edge raised: what networkx says is what an own Edmonds-Karp says, and off the cut nothing moves.
        def raised(edge, by):
            return [(u, v, cap + by if (u, v) == edge else cap) for u, v, cap in edges]

        sweep = result(c, rid, "sweep")
        cut_edges = {(u, v) for u, v, _ in cut["cut_edges"]}
        assert len(sweep["rows"]) == len(edges)
        for row in sweep["rows"]:
            edge = tuple(row["edge"])
            assert row["gain"] == edmonds_karp(nodes, raised(edge, 10)) - reference
            assert row["on_cut"] == (edge in cut_edges)
            if edge not in cut_edges:
                assert row["gain"] == 0, row
        gains = [r["gain"] for r in sweep["rows"] if r["on_cut"]]
        assert max(gains) > 0  # the default seed has a bottleneck that does give

        # The comparison Card picks one edge on the cut that gives and one off it that does not.
        whatif = result(c, rid, "whatif")
        on, off, *rest = whatif["rows"]
        assert rest and all(r["on_cut"] and r["gain"] == 0 for r in rest)  # another minimum cut avoids this one
        assert count >= 2
        assert on["on_cut"] and on["gain"] == max(gains)
        assert not off["on_cut"] and off["gain"] == 0 and off["value"] == reference

        # Rerun just that Card, a bigger raise on a named edge off the cut: still nothing.
        name = "->".join(off["edge"])
        edit_card(env, c, "whatif", lambda b: (set_param(b, "edge", name), set_param(b, "raise_by", 1000)))
        rid2 = run_ok(c, "whatif", ["whatif"], propagate=False)
        first = result(c, rid2, "whatif")["rows"][0]
        assert first["edge"] == off["edge"] and first["gain"] == 0
        assert first["gain"] == edmonds_karp(nodes, raised(tuple(off["edge"]), 1000)) - reference

        # A bigger raise on the cut edge moves it the most it can, up to the next cut.
        edit_card(env, c, "whatif", lambda b: (set_param(b, "edge", "->".join(on["edge"])), set_param(b, "raise_by", 1000)))
        rid3 = run_ok(c, "whatif", ["whatif"], propagate=False)
        first = result(c, rid3, "whatif")["rows"][0]
        assert first["gain"] == edmonds_karp(nodes, raised(tuple(on["edge"]), 1000)) - reference
        assert first["gain"] >= on["gain"]

        # Another seed is another network, by the same checks.
        edit_card(env, c, "network", lambda b: set_param(b, "seed", 11))
        rid4 = run_ok(c, "start", FLOW)
        net4 = result(c, rid4, "network")
        assert net4["edges"] != net["edges"]
        assert result(c, rid4, "flow")["value"] == edmonds_karp(net4["nodes"], [tuple(e) for e in net4["edges"]])
    finally:
        env.cleanup()
