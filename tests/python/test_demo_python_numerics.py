"""The demos newton, pharmacokinetics and max-flow, run whole and checked against
references computed here in plain Python, without the Cards' code or libraries:

- newton: the exact cube roots of unity, the order and iteration counts of an own
  Newton iteration, the 3-fold symmetry of the basins; damping 0.5 turns order 2
  into order 1 with ratio 1/2, and `offset` closer to the critical point costs more
  iterations.
- pharmacokinetics: an own RK4 integration of the same ODE (peak, trough,
  accumulation ratio); doubling the half-life raises the accumulation, and a second
  missed dose lengthens the time below the window.
- max-flow: an own Edmonds-Karp and a brute force over all cuts on the network the
  Card generated; raising an edge off the cut gains nothing.

Each also reruns one Card alone (propagate=False) with a changed param. PNG displays
and Plotly JSON are saved under TSUGUMI_DEMO_FIGURES when it is set, for looking at.
"""

from __future__ import annotations

import cmath
import itertools
import json
import math
import os
from collections import deque
from pathlib import Path

import pytest

from conftest import demo_environment
from projects import start_run, wait_run
from test_demos import connect, display_text, edit_card, set_param, stderr
from tsugumi_client import path, text
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


# ---------------------------------------------------------------- newton

NEWTON = ["roots", "orbits", "order", "basins", "trouble"]


def newton_reference(coeffs, z0, damping=1.0, tolerance=1e-12, max_iter=60):
    """The iterates, with the Cards' stopping rule (a step below tolerance, or p(z)
    below its rounding noise), written with Horner's rule on Python complex numbers."""
    def horner(cs, z):
        out = 0j
        for a in cs:
            out = out * z + a
        return out

    degree = len(coeffs) - 1
    derivative = [a * (degree - i) for i, a in enumerate(coeffs[:-1])]
    magnitudes = [abs(a) for a in coeffs]
    zs = [complex(z0)]
    z = complex(z0)
    for _ in range(max_iter):
        value = horner(coeffs, z)
        if abs(value) <= 4 * 2.220446049250313e-16 * abs(horner(magnitudes, abs(z))):
            return zs, True
        step = damping * value / horner(derivative, z)
        z -= step
        zs.append(z)
        if abs(step) < tolerance:
            return zs, True
    return zs, False


CUBE = [cmath.exp(2j * math.pi * k / 3) for k in range(3)]


def test_newton(tmp_path):
    env = demo_environment(tmp_path, "newton")
    try:
        c = connect(env)
        rid = run_ok(c, "start", NEWTON)
        save_figures(c, rid, "newton", NEWTON)
        assert_no_missing_glyphs(c, rid, ["orbits", "basins", "trouble"])

        # The roots are the exact cube roots of unity.
        roots = result(c, rid, "roots")["roots"]
        assert len(roots) == 3
        for exact in CUBE:
            assert min(abs(complex(r[0], r[1]) - exact) for r in roots) < 1e-12

        # Each start goes where a plain iteration goes, at the same speed and the theoretical order.
        rows = result(c, rid, "order")
        card = json.loads((tmp_path / "newton/definitions/orbits/_card.json").read_text())
        starts = dict((p["name"], p["data"]) for p in card["params"])
        assert len(rows) == len(starts["start_re"])
        for row, re_, im in zip(rows, starts["start_re"], starts["start_im"]):
            zs, converged = newton_reference([1.0, 0.0, 0.0, -1.0], complex(re_, im))
            assert converged and row["status"] == "converged"
            target = min(CUBE, key=lambda r: abs(r - zs[-1]))
            assert abs(complex(*row["root"]) - target) < 1e-12
            assert row["iterations"] == len(zs) - 1
            assert row["q"] == pytest.approx(2.0, abs=0.25), row
            assert row["constant"] == pytest.approx(1.0, abs=0.35), row  # |p''/(2p')| = 1/|r| = 1

        # The near-critical start is the slowest of the list.
        slow = next(r for r in rows if r["z0"] == [0.05, 0.03])
        assert slow["iterations"] > 2 * min(r["iterations"] for r in rows)

        # The basins: real coefficients make the two complex roots mirror each other exactly
        # (the square view is not 3-fold symmetric, so the real root's share differs), and an
        # own iteration on a coarse grid gives about the same shares.
        basins = result(c, rid, "basins")
        assert basins["shares"][0] == pytest.approx(basins["shares"][1], abs=1e-9), basins
        assert basins["unconverged"] < 0.01
        coarse = [0, 0, 0]
        for i in range(81):
            for j in range(81):
                end = newton_reference([1.0, 0.0, 0.0, -1.0], complex(-2 + i * 0.05, -2 + j * 0.05) + 0.001j)[0][-1]
                coarse[min(range(3), key=lambda k: abs(complex(*roots[k][:2]) - end))] += 1
        for own, shown in zip(coarse, basins["shares"]):
            assert own / 81 ** 2 == pytest.approx(shown, abs=0.02), (coarse, basins)

        # The failures: order 1 with ratio 1/2 at a double root, order 2 back with lambda = 2, a 2-cycle.
        trouble = result(c, rid, "trouble")
        by_name = {r["name"]: r for r in trouble}
        double = next(r for n, r in by_name.items() if n.startswith("重根 "))
        assert double["status"] == "converged" and double["q"] == pytest.approx(1.0, abs=0.05)
        assert double["rate"] == pytest.approx(0.5, abs=0.02)
        assert 1e-10 < double["last_error"] < 1e-5  # about sqrt(eps): the limit said on the note
        fixed = next(r for n, r in by_name.items() if "λ" in n or "lambda" in n)
        assert fixed["q"] == pytest.approx(2.0, abs=0.3) and fixed["iterations"] < double["iterations"] / 2
        cycle = next(r for n, r in by_name.items() if n.startswith("閉路"))
        assert cycle["status"] == "max_iter" and cycle["iterations"] == 60
        near = next(r for n, r in by_name.items() if n.startswith("臨界点"))
        assert near["peak_error"] == pytest.approx(1 / (3 * 1e-3 ** 2), rel=0.01)  # the first step lands at 1 / (3 z0^2)
        assert near["rate"] == pytest.approx(2 / 3, abs=0.01)  # far away, each step keeps 2/3 of z
        assert near["iterations"] == len(newton_reference([1.0, 0.0, 0.0, -1.0], 1e-3)[0]) - 1

        # Closer to the critical point, a longer way back: rerun this Card alone.
        edit_card(env, c, "trouble", lambda b: set_param(b, "offset", 1e-5))
        rid2 = run_ok(c, "trouble", ["trouble"], propagate=False)
        nearer = next(r for r in result(c, rid2, "trouble") if r["name"].startswith("臨界点"))
        assert nearer["iterations"] == len(newton_reference([1.0, 0.0, 0.0, -1.0], 1e-5)[0]) - 1
        assert nearer["iterations"] > near["iterations"] + 15  # log(1e4) / log(1.5) = 22.7 more steps

        # Zooming in reruns just the map.
        edit_card(env, c, "basins", lambda b: (set_param(b, "grid", 200), set_param(b, "half_width", 0.2),
                                               set_param(b, "center_re", -0.8), set_param(b, "center_im", 0.0)))
        rid3 = run_ok(c, "basins", ["basins"], propagate=False)
        save_figures(c, rid3, "newton", ["basins"], "-zoom")
        edit_card(env, c, "basins", lambda b: (set_param(b, "grid", 600), set_param(b, "half_width", 2.0),
                                               set_param(b, "center_re", 0.0)))

        # Damping from settings: order 1 with ratio |1 - lambda| = 1/2 everywhere.
        edit_card(env, c, "settings", lambda b: set_param(b, "damping", 0.5))
        rid4 = run_ok(c, "start", NEWTON)
        save_figures(c, rid4, "newton", ["orbits", "order", "basins"], "-damped")
        for row in result(c, rid4, "order"):
            assert row["q"] == pytest.approx(1.0, abs=0.05), row
            assert row["ratio"] == pytest.approx(0.5, abs=0.02), row

        # A multiple root in settings: the table reads multiplicity 2 and order 1.
        edit_card(env, c, "settings", lambda b: (set_param(b, "damping", 1.0), set_param(b, "coeffs", [1.0, 0.0, -3.0, 2.0])))
        rid5 = run_ok(c, "start", NEWTON)
        save_figures(c, rid5, "newton", ["roots", "orbits", "order", "basins"], "-double")
        assert sorted(r[2] for r in result(c, rid5, "roots")["roots"]) == [1, 2]
        doubled = [r for r in result(c, rid5, "order") if r["m"] == 2 and r["q"] is not None]
        assert doubled and all(r["q"] == pytest.approx(1.0, abs=0.1) for r in doubled), doubled

        # The black regions of a stable 2-cycle: a whole neighbourhood of 0 and 1 never converges.
        edit_card(env, c, "settings", lambda b: set_param(b, "coeffs", [1.0, 0.0, -2.0, 2.0]))
        rid6 = run_ok(c, "start", NEWTON)
        save_figures(c, rid6, "newton", ["basins"], "-cycle")
        assert result(c, rid6, "basins")["unconverged"] > 0.001
        assert newton_reference([1.0, 0.0, -2.0, 2.0], 0.01)[1] is False  # the plain iteration stays on the cycle too
    finally:
        env.cleanup()


# ---------------------------------------------------------------- pharmacokinetics

PK = ["simulate", "check", "window", "missed"]


def pk_reference(dose, tau, half_life, ka, volume, fraction, end, h=0.01):
    """Plain RK4 of the same ODE: (t, C) samples and the C just before each dose."""
    ke = math.log(2) / half_life

    def rhs(a, conc):
        return -ka * a, ka * a / volume - ke * conc

    a = conc = 0.0
    samples = []
    troughs = []
    t = 0.0
    n = 0
    steps_per_interval = int(round(tau / h))
    total_steps = int(round(end / h))
    for step in range(total_steps + 1):
        if step % steps_per_interval == 0 and step < total_steps:
            if step:
                troughs.append(conc)
            a += fraction * dose
            n += 1
        samples.append((t, conc))
        if step == total_steps:
            break
        k1 = rhs(a, conc)
        k2 = rhs(a + h / 2 * k1[0], conc + h / 2 * k1[1])
        k3 = rhs(a + h / 2 * k2[0], conc + h / 2 * k2[1])
        k4 = rhs(a + h * k3[0], conc + h * k3[1])
        a += h / 6 * (k1[0] + 2 * k2[0] + 2 * k3[0] + k4[0])
        conc += h / 6 * (k1[1] + 2 * k2[1] + 2 * k3[1] + k4[1])
        t = (step + 1) * h
    return samples, troughs, n


def test_pharmacokinetics(tmp_path):
    env = demo_environment(tmp_path, "pharmacokinetics")
    try:
        c = connect(env)
        rid = run_ok(c, "start", PK)
        save_figures(c, rid, "pharmacokinetics", PK)
        assert_no_missing_glyphs(c, rid, ["window", "missed"])

        settings = {p["name"]: p["data"] for p in json.loads((tmp_path / "pharmacokinetics/definitions/settings/_card.json").read_text())["params"]}

        # The interactive plot: one panel per regimen, each with the window band, on one scale.
        shown = c.call("RunRecordRequest", runId=rid, card=path("simulate")).record.display
        assert shown.valueType == 7 and text(shown.value.module).endswith("-tsugumi-view-plotly")
        fig = json.loads(text(shown.value.configJson))
        assert len(fig["data"]) == len(settings["doses_mg"]) == len(fig["layout"]["shapes"])
        assert "架空" in fig["layout"]["title"]["text"]
        ranges = [v["range"] for k, v in fig["layout"].items() if k.startswith("yaxis")]
        assert len(ranges) == len(settings["doses_mg"]) and all(r == ranges[0] for r in ranges)

        args = (settings["half_life_h"], settings["ka_per_h"], settings["volume_l"], settings["bioavailability"], settings["days"] * 24)
        checks = result(c, rid, "check")
        windows = result(c, rid, "window")
        assert len(checks) == len(windows) == len(settings["doses_mg"])

        for dose, tau, chk, win in zip(settings["doses_mg"], settings["intervals_h"], checks, windows):
            samples, troughs, doses = pk_reference(dose, tau, *args)
            last_interval = [v for t, v in samples if (doses - 2) * tau <= t <= (doses - 1) * tau]
            assert win["peak"] == pytest.approx(max(last_interval), rel=1e-3)
            assert win["trough"] == pytest.approx(min(last_interval), rel=1e-3)
            assert chk["ratio_numeric"] == pytest.approx(troughs[-1] / troughs[0], rel=1e-4)
            # The accumulation ratio settled at steady state.
            assert chk["reached"] > 0.999 and chk["ratio_numeric"] == pytest.approx(chk["ratio_exact"], rel=1e-2)
            assert chk["gap"] < 1e-4
            # The steady-state mean follows from the dose rate alone: F D / (ke V tau).
            ke = math.log(2) / settings["half_life_h"]
            assert chk["mean_numeric"] == pytest.approx(settings["bioavailability"] * dose / (ke * settings["volume_l"] * tau), rel=2e-3)

        # The same daily dose split finer: the same mean, a smaller swing.
        once, twice, four = windows[0], windows[1], windows[2]
        assert once["peak"] > twice["peak"] > four["peak"] and once["trough"] < twice["trough"] < four["trough"]
        assert four["inside"] >= twice["inside"] > once["inside"]

        # A second dose missed lengthens the time below the window: rerun this Card alone.
        missed = result(c, rid, "missed")
        edit_card(env, c, "missed", lambda b: set_param(b, "missed_count", 2))
        rid2 = run_ok(c, "missed", ["missed"], propagate=False)
        save_figures(c, rid2, "pharmacokinetics", ["missed"], "-two")
        missed2 = result(c, rid2, "missed")
        for one, two in zip(missed, missed2):
            assert len(two["skipped"]) == 2
            assert two["lowest"] <= one["lowest"] + 1e-9
            assert two["below_missed_h"] >= one["below_missed_h"]
        assert any(two["below_missed_h"] > one["below_missed_h"] for one, two in zip(missed, missed2))

        # A longer half-life accumulates more, against R = 1 / (1 - exp(-ke tau)) from the note.
        edit_card(env, c, "settings", lambda b: set_param(b, "half_life_h", 16.0))
        rid3 = run_ok(c, "start", PK)
        save_figures(c, rid3, "pharmacokinetics", ["simulate", "window"], "-long")
        for before, after in zip(checks, result(c, rid3, "check")):
            assert after["ratio_numeric"] > before["ratio_numeric"]
            r = 1 / (1 - math.exp(-math.log(2) / 16.0 * after["tau"]))
            assert after["ratio_numeric"] == pytest.approx(r, rel=0.05)
    finally:
        env.cleanup()


# ---------------------------------------------------------------- max-flow

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
        edit_card(env, c, "settings", lambda b: set_param(b, "seed", 11))
        rid4 = run_ok(c, "start", FLOW)
        net4 = result(c, rid4, "network")
        assert net4["edges"] != net["edges"]
        assert result(c, rid4, "flow")["value"] == edmonds_karp(net4["nodes"], [tuple(e) for e in net4["edges"]])
    finally:
        env.cleanup()
