"""Check the maximum flow and minimum cut against independent implementations."""

from __future__ import annotations

import itertools
import json
import math
import os
from collections import deque

import pytest

from conftest import demo_environment
from projects import start_run, wait_run
from harness.document import set_param
from harness.observations import read_pages
from test_demos import connect, recorded_output, stderr
from tsugumi_client import path, text
from Tsugumi.Data.RunState import RunState

pytestmark = pytest.mark.skipif(os.environ.get("TSUGUMI_TEST_DEMO_RUNS") != "1",
                                reason="runs the demo (large Nix closures): TSUGUMI_TEST_DEMO_RUNS=1")


def result(c, run_id, card):
    artifacts = c.call("ArtifactListRequest", runId=run_id, card=path(card)).artifacts
    artifact = next(row for row in artifacts if text(row.output) == "result")
    return json.loads(read_pages(c, "ArtifactReadRequest", artifactId=text(artifact.artifactId),
                                 member="result.json", page_bytes=65536))


def network(tmp_path, c, run_id):
    out = recorded_output(tmp_path, c, run_id, "network", port="network")
    nodes = out["nodes"].split()
    return nodes, [(nodes[i], nodes[j], cap)
                   for i, j, cap in zip(out["tail"], out["head"], out["capacity"])]


def run_ok(c, card, cards, propagate=True):
    rid = start_run(c, [card], propagate=propagate)
    st = wait_run(c, rid)
    assert st.state == RunState.succeeded, (st.failure and text(st.failure.diagnostic), {x: stderr(c, rid, x)[-1500:] for x in cards})
    return rid


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
        nodes, edges = network(tmp_path / "network", c, rid)
        reference = edmonds_karp(nodes, edges)
        best, count = brute_cuts(nodes, edges)
        assert reference == best

        flow = recorded_output(tmp_path / "flow", c, rid, "flow", port="flow")
        cut = recorded_output(tmp_path / "cut", c, rid, "cut", port="cut")
        side = set(cut["source_side"].split())
        crossing = [(u, v, cap) for u, v, cap in edges if u in side and v not in side]
        verify = result(c, rid, "verify")
        assert flow["value"] == cut["value"] == reference
        assert verify["networkx"] == verify["scipy"] == reference
        assert len(verify["minimum_cuts"]) == count
        assert sum(e[2] for e in crossing) == reference

        # Every edge raised: what networkx says is what an own Edmonds-Karp says, and off the cut nothing moves.
        def raised(edge, by):
            return [(u, v, cap + by if (u, v) == edge else cap) for u, v, cap in edges]

        sweep = result(c, rid, "sweep")
        cut_edges = {(u, v) for u, v, _ in crossing}
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
        set_param(c, "whatif", "edge", name)
        set_param(c, "whatif", "raise_by", 1000)
        rid2 = run_ok(c, "whatif", ["whatif"], propagate=False)
        first = result(c, rid2, "whatif")["rows"][0]
        assert first["edge"] == off["edge"] and first["gain"] == 0
        assert first["gain"] == edmonds_karp(nodes, raised(tuple(off["edge"]), 1000)) - reference

        # A bigger raise on the cut edge moves it the most it can, up to the next cut.
        set_param(c, "whatif", "edge", "->".join(on["edge"]))
        rid3 = run_ok(c, "whatif", ["whatif"], propagate=False)
        first = result(c, rid3, "whatif")["rows"][0]
        assert first["gain"] == edmonds_karp(nodes, raised(tuple(on["edge"]), 1000)) - reference
        assert first["gain"] >= on["gain"]

        # Another seed is another network, by the same checks.
        set_param(c, "network", "seed", 11)
        rid4 = run_ok(c, "start", FLOW)
        nodes4, edges4 = network(tmp_path / "new-network", c, rid4)
        assert edges4 != edges
        flow4 = recorded_output(tmp_path / "new-flow", c, rid4, "flow", port="flow")
        assert flow4["value"] == edmonds_karp(nodes4, edges4)
    finally:
        env.cleanup()
