"""Three laboratory demos in Python, each against a reference the Cards do not compute.

- dose-response: the EC50 and Hill slope of `fit` against a separate grid least
  squares on the same plate (numpy only, no curve_fit), the true values inside
  the intervals of `bootstrap`, wider intervals with more noise, and the warning
  when the range does not bracket the EC50.
- molecules: molecular weights from atomic masses and hand-written formulas, N + O
  from those formulas, TPSA summed by hand from Ertl's fragment contributions, the
  rule of five worked out from those numbers; invalid SMILES get a message, not a crash.
- cell-count: precision and recall recomputed from the logged centres with a
  separate bipartite matching, the number of true touching pairs measured from
  the true centres, watershed beating the threshold alone, the method failing at
  high density, and `sweep` (a copy of the steps) giving the numbers of `score`.
"""

from __future__ import annotations

import json
import os
import re

import pytest

from conftest import demo_environment
from projects import start_run, wait_run
from test_demos import connect, display_text, edit_card, set_param, stderr
from tsugumi_client import path, text
from Tsugumi.Data.RunState import RunState

np = pytest.importorskip("numpy")

pytestmark = pytest.mark.skipif(os.environ.get("TSUGUMI_TEST_DEMO_RUNS") != "1",
                                reason="runs the demo (large Nix closures): TSUGUMI_TEST_DEMO_RUNS=1")


def run(c, card="start", propagate=True, cards=()):
    rid = start_run(c, [card], propagate=propagate)
    st = wait_run(c, rid)
    assert st.state == RunState.succeeded, (st.failure and text(st.failure.diagnostic), [stderr(c, rid, x) for x in cards])
    return rid


def stdout(c, run_id, card):
    return text(c.call("LogRangeRequest", runId=run_id, card=path(card), stream=1, limitBytes=262144).text)


def logged(c, run_id, card, prefix):
    """The JSON a Card printed after `prefix` on a line of its log."""
    line = next(x for x in stdout(c, run_id, card).splitlines() if x.startswith(prefix + " "))
    return json.loads(line[len(prefix) + 1:])


def table_rows(markdown):
    """Rows of the Markdown table(s) in a display, as lists of cells; header and rule left out."""
    rows = [[cell.strip() for cell in line.strip("|").split("|")] for line in markdown.splitlines() if line.startswith("|")]
    return rows[2:]


# -- dose-response -------------------------------------------------------------


def reference_fit(x, y):
    """Least squares by zooming grids over log10 EC50 and Hill slope; for each
    pair the two plateaus are linear, so they are solved exactly."""
    log_x = np.log10(x)
    centre = np.array([log_x.mean(), 1.5])
    half = np.array([log_x.max() - log_x.min(), 1.4])
    best = None
    for _ in range(12):
        for le in np.linspace(centre[0] - half[0], centre[0] + half[0], 41):
            for h in np.linspace(max(centre[1] - half[1], 0.1), centre[1] + half[1], 41):
                s = 1 / (1 + 10 ** ((le - log_x) * h))
                a = np.column_stack([1 - s, s])
                coef = np.linalg.solve(a.T @ a, a.T @ y)
                sse = np.sum((y - a @ coef) ** 2)
                if best is None or sse < best[0]:
                    best = (sse, le, h)
        centre = np.array(best[1:])
        half = half / 4
    return 10 ** best[1], best[2]


def interval(cell):
    low, high = cell.split("–")
    return float(low), float(high)


def test_dose_response(tmp_path):
    env = demo_environment(tmp_path, "dose-response")
    try:
        c = connect(env)
        cards = ("generate", "fit", "bootstrap", "compare", "range")
        rid = run(c, cards=cards)

        # The fit agrees with a separate least squares on the same plate.
        plate = logged(c, rid, "generate", "plate")
        conc = np.array(plate["conc"])
        response = np.array(plate["response"])
        assert conc.size == 8 and response.shape == (2, 8, 3)
        fit_log = stdout(c, rid, "fit")
        for i, name in enumerate("AB"):
            ec50, hill = reference_fit(np.repeat(conc, 3), response[i].ravel())
            m = re.search(rf"^{name}: .* EC50 (\S+) uM hill (\S+) ", fit_log, re.M)
            assert float(m[1]) == pytest.approx(ec50, rel=0.01)
            assert float(m[2]) == pytest.approx(hill, rel=0.01)

        # The true parameters (those of the generate Card) lie inside the intervals.
        shown = display_text(c, rid, "bootstrap")
        rows = table_rows(shown)
        truth = {("A", "EC50 (µM)"): 0.3, ("A", "Hill 勾配"): 1.0, ("B", "EC50 (µM)"): 4.5, ("B", "Hill 勾配"): 1.6}
        assert len(rows) == 4
        base_width = {}
        for name, what, true, _, ci, _, verdict in rows:
            low, high = interval(ci)
            assert low <= truth[(name, what)] <= high and float(true) == truth[(name, what)] and verdict == "含む"
            base_width[(name, what)] = high / low
        assert "すべての真の値が区間に入りました" in shown and "濃度範囲が EC50 を挟んでいません" not in shown

        # Relative potency: the true ratio is 15, inside the interval.
        m = re.search(r"relative potency B/A (\S+) \[(\S+), (\S+)\] true 15", stdout(c, rid, "compare"))
        assert float(m[2]) <= 15 <= float(m[3]) and float(m[2]) <= float(m[1]) <= float(m[3])

        # Narrow when the range brackets the EC50 well, blown up when it does not.
        sweep = logged(c, rid, "range", "interval widths")
        at = {top: i for i, top in enumerate(sweep["tops"])}
        assert sweep["widths"]["B"][at[100.0]] < 3 and sweep["widths"]["B"][at[1.0]] > 100
        assert sweep["widths"]["A"][at[10.0]] < 3 and sweep["widths"]["A"][at[0.03]] > 100

        # One Card alone, on its saved inputs: fewer resamples, the same estimates.
        edit_card(env, c, "bootstrap", lambda b: set_param(b, "n_boot", 100))
        rid2 = run(c, "bootstrap", propagate=False, cards=cards)
        again = display_text(c, rid2, "bootstrap")
        assert "100 回" in again
        assert [r[3] for r in table_rows(again)] == [r[3] for r in rows]
        edit_card(env, c, "bootstrap", lambda b: set_param(b, "n_boot", 500))

        # More noise: wider intervals.
        edit_card(env, c, "generate", lambda b: set_param(b, "noise_sd", 12.0))
        rid3 = run(c, "generate", cards=cards)
        for name, what, _, _, ci, _, _ in table_rows(display_text(c, rid3, "bootstrap")):
            low, high = interval(ci)
            assert high / low > base_width[(name, what)]

        # A top concentration below B's EC50: the Card says so and the interval blows up.
        def short_range(b):
            set_param(b, "noise_sd", 4.0)
            set_param(b, "top_um", 1.0)
        edit_card(env, c, "generate", short_range)
        rid4 = run(c, "generate", cards=cards)
        warned = display_text(c, rid4, "bootstrap")
        assert "**B: 濃度範囲が EC50 を挟んでいません。**" in warned
        assert "**A: 区間が測定範囲の外に出ています。**" in warned  # near the end of the range, but still bracketed
        b_row = next(r for r in table_rows(warned) if r[:2] == ["B", "EC50 (µM)"])
        low, high = interval(b_row[4])
        assert high / low > 100 * base_width[("B", "EC50 (µM)")]
    finally:
        env.cleanup()


# -- molecules -----------------------------------------------------------------

MASSES = {"C": 12.011, "H": 1.008, "N": 14.007, "O": 15.999, "F": 18.998, "S": 32.06, "Cl": 35.45}

# Molecular formulas from the chemistry literature, not from RDKit.
FORMULAS = {
    "aspirin": "C9H8O4", "salicylic acid": "C7H6O3", "caffeine": "C8H10N4O2", "ibuprofen": "C13H18O2",
    "paracetamol": "C8H9NO2", "nicotine": "C10H14N2", "morphine": "C17H19NO3", "codeine": "C18H21NO3",
    "diazepam": "C16H13ClN2O", "penicillin G": "C16H18N2O4S", "glucose": "C6H12O6", "cholesterol": "C27H46O",
    "atorvastatin": "C33H35FN2O5", "cyclosporin A": "C62H111N11O12", "dopamine": "C8H11NO2",
}

# TPSA by hand from Ertl, Rohde & Selzer (J. Med. Chem. 2000): O= 17.07, OH 20.23, ester/ether O 9.23,
# aromatic N with three substituents 4.93, aromatic N with two 12.89.
TPSA = {
    "aspirin": 17.07 + 20.23 + 17.07 + 9.23,
    "caffeine": 3 * 4.93 + 12.89 + 2 * 17.07,
    "glucose": 5 * 20.23 + 9.23,
    "ibuprofen": 17.07 + 20.23,
    "cholesterol": 20.23,
}


def atoms(formula):
    return {el: int(n or 1) for el, n in re.findall(r"([A-Z][a-z]?)(\d*)", formula)}


def molecular_weight(formula):
    return sum(MASSES[el] * n for el, n in atoms(formula).items())


def test_molecules(tmp_path):
    env = demo_environment(tmp_path, "molecules")
    try:
        c = connect(env)
        cards = ("properties", "structures", "lipinski", "chemspace", "similarity")
        rid = run(c, cards=cards)

        rows = {r[0]: r for r in table_rows(display_text(c, rid, "properties"))}
        assert set(rows) == set(FORMULAS)
        for name, formula in FORMULAS.items():
            assert rows[name][1] == formula
            assert float(rows[name][2]) == pytest.approx(molecular_weight(formula), abs=0.03), name
        for name, tpsa in TPSA.items():
            assert float(rows[name][4]) == pytest.approx(tpsa, abs=0.01), name

        # The rule of five, worked out from the formulas and the Card's logP and NH+OH.
        checked = {r[0]: r for r in table_rows(display_text(c, rid, "lipinski"))}
        number = lambda s: float(s.strip("*"))
        for name, formula in FORMULAS.items():
            count = atoms(formula)
            n_o = count.get("N", 0) + count.get("O", 0)
            assert number(checked[name][4]) == n_o, name
            broken = (molecular_weight(formula) > 500) + (number(checked[name][2]) > 5) + (number(checked[name][3]) > 5) + (n_o > 10)
            assert int(checked[name][5]) == broken, name
        assert {n for n, r in checked.items() if r[6] == "**不通過**"} == {"atorvastatin", "cyclosporin A"}
        assert number(checked["cholesterol"][2]) > 5 and checked["cholesterol"][6] == "通過"

        # The plot says what it should in its hover text.
        figure = json.loads(text(c.call("RunRecordRequest", runId=rid, card=path("chemspace")).record.display.value.configJson))
        failed = next(t for t in figure["data"] if t.get("name") == "不通過")
        assert sorted(row[0] for row in failed["customdata"]) == ["atorvastatin", "cyclosporin A"]
        assert sorted(failed["text"]) == ["atorvastatin", "cyclosporin A"] and any("違反" in row[1] for row in failed["customdata"])

        # Similarity: analogues are near, unrelated molecules far; a coarser fingerprint looks more alike.
        log = stdout(c, rid, "similarity")
        m = re.search(r"mean similarity (\S+); most similar (.+) and (.+) (\S+)$", log, re.M)
        assert {m[2], m[3]} == {"morphine", "codeine"} and float(m[4]) > 0.5
        mean = float(m[1])
        edit_card(env, c, "similarity", lambda b: set_param(b, "radius", 0))
        rid2 = run(c, "similarity", propagate=False, cards=cards)
        assert float(re.search(r"mean similarity (\S+);", stdout(c, rid2, "similarity"))[1]) > mean

        # The strictest reading of the rule, one Card alone: cholesterol no longer passes.
        edit_card(env, c, "lipinski", lambda b: set_param(b, "max_violations", 0))
        rid3 = run(c, "lipinski", propagate=False, cards=cards)
        strict = {r[0]: r for r in table_rows(display_text(c, rid3, "lipinski"))}
        assert strict["cholesterol"][6] == "**不通過**" and strict["aspirin"][6] == "通過"

        # A new SMILES pasted on the canvas goes through every Card.
        def paste(name, smiles):
            def f(b):
                set_param(b, "name", name)
                set_param(b, "smiles", smiles)
            edit_card(env, c, "paste", f)
        paste("toluene", "Cc1ccccc1")
        rid4 = run(c, "paste", cards=cards)
        rows = {r[0]: r for r in table_rows(display_text(c, rid4, "properties"))}
        assert float(rows["toluene"][2]) == pytest.approx(molecular_weight("C7H8"), abs=0.03) and "dopamine" not in rows

        # Not a SMILES, and a valence no atom can have: a message on the Card, the others computed.
        for smiles, why in (("C1CC(", "文法"), ("C(C)(C)(C)(C)C", "化学")):
            paste("broken", smiles)
            rid5 = run(c, "paste", cards=cards)
            shown = display_text(c, rid5, "properties")
            assert f"**broken** の SMILES `{smiles}` は計算から外しました" in shown and why in shown
            assert "broken" not in {r[0] for r in table_rows(shown)} and "aspirin" in shown

        # Nothing readable at all: a failure with the reason.
        paste("broken", "C1CC(")
        edit_card(env, c, "properties", lambda b: set_param(b, "molecules", []))
        rid6 = start_run(c, ["paste"])
        st = wait_run(c, rid6)
        assert st.state == RunState.failed and "no readable molecule" in text(st.failure.diagnostic)
    finally:
        env.cleanup()


# -- cell-count ----------------------------------------------------------------


def bipartite_matching(found, truth, gate):
    """Kuhn's augmenting paths: the largest one-to-one matching within `gate`."""
    near = [[j for j, t in enumerate(truth) if (f[0] - t[0]) ** 2 + (f[1] - t[1]) ** 2 <= gate ** 2] for f in found]
    owner = {}

    def augment(i, seen):
        for j in near[i]:
            if j not in seen:
                seen.add(j)
                if j not in owner or augment(owner[j], seen):
                    owner[j] = i
                    return True
        return False

    return sum(augment(i, set()) for i in range(len(found)))


def check_scores(report, method):
    truth = list(zip(report["true_x"], report["true_y"]))
    r = report[method]
    found = list(zip(r["found_x"], r["found_y"]))
    assert r["count"] == len(found)
    matched = bipartite_matching(found, truth, report["gate"])
    # Rounded centres can move a borderline pair over the gate: at most one or two.
    assert abs(matched - r["matched"]) <= 2
    assert r["precision"] == pytest.approx(r["matched"] / len(found))
    assert r["recall"] == pytest.approx(r["matched"] / len(truth))
    return r


def test_cell_count(tmp_path):
    env = demo_environment(tmp_path, "cell-count")
    try:
        c = connect(env)
        cards = ("generate", "preprocess", "segment", "score", "sweep", "mitosis", "real")
        rid = run(c, cards=cards)

        report = logged(c, rid, "score", "scores")
        assert report["cells"] == 80 and len(report["true_x"]) == 80
        # The true centres hold at least the 16 touching pairs asked for (centres closer than two radii).
        xy = np.column_stack([report["true_x"], report["true_y"]])
        d = np.linalg.norm(xy[:, None] - xy[None], axis=2)
        assert np.sum(np.triu(d < 2 * 9.0, 1)) >= 16

        plain = check_scores(report, "plain")
        split = check_scores(report, "split")
        # Splitting finds the touching cells the threshold merged, without inventing many.
        assert abs(split["count"] - 80) < abs(plain["count"] - 80)
        assert split["recall"] > plain["recall"] + 0.1
        assert split["f1"] > plain["f1"] and split["f1"] > 0.9 and split["precision"] > 0.9

        # The sweep's copy of the steps gives score's numbers at generate's settings.
        sweep = logged(c, rid, "sweep", "sweep")
        i, j = sweep["noises"].index(0.1), sweep["cells"].index(80)
        assert sweep["plain"][i][j] == pytest.approx(plain["f1"])
        assert sweep["split"][i][j] == pytest.approx(split["f1"])
        # Crowded: even watershed fails, and the threshold alone much sooner.
        last = sweep["cells"].index(400)
        assert sweep["split"][i][last] < 0.6 and sweep["plain"][i][last] < sweep["split"][i][last]
        assert all(sweep["split"][i][k] >= sweep["plain"][i][k] for k in range(len(sweep["cells"])))
        assert sweep["split_count"][i][last] < 400 * 0.7  # crowded: too few objects
        loud, few = sweep["noises"].index(1.0), sweep["cells"].index(40)
        assert sweep["split_count"][loud][few] > 3 * 40 and sweep["split"][loud][few] < 0.5  # noise: too many

        # The real image: no truth, so only that the count is sensible and splitting adds to it.
        m = re.search(r"threshold only (\d+) objects; watershed (\d+) objects", stdout(c, rid, "real"))
        assert 150 < int(m[1]) < int(m[2]) < 500

        # One Card alone, on its saved input: seeds farther apart split less.
        before = int(re.search(r"watershed (\d+) objects", stdout(c, rid, "segment"))[1])
        edit_card(env, c, "segment", lambda b: set_param(b, "min_distance", 15))
        rid2 = run(c, "segment", propagate=False, cards=cards)
        assert int(re.search(r"watershed (\d+) objects", stdout(c, rid2, "segment"))[1]) < before
        edit_card(env, c, "segment", lambda b: set_param(b, "min_distance", 5))

        # Denser, through to the score: recall falls.
        edit_card(env, c, "generate", lambda b: set_param(b, "cells", 320))
        rid3 = run(c, "generate", cards=cards)
        crowded = logged(c, rid3, "score", "scores")
        assert crowded["cells"] == 320
        assert check_scores(crowded, "split")["recall"] < split["recall"]
    finally:
        env.cleanup()
