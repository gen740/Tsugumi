"""The R demos, checked against references written here rather than the Cards' code.

- survival: the Kaplan-Meier curves, the medians, the log-rank statistic and a one-covariate
  Cox model (Efron's ties, Newton's method) by hand on the data the `load` Card exports, plus
  the values printed by R's documentation for `lung`.
- meta-analysis: effect sizes from the trial counts, inverse-variance fixed effect and
  DerSimonian-Laird random effects by hand, leave-one-out by hand, the published REML values.
- rna-seq: median-of-ratios size factors by hand, within-cell-line fold changes of known
  responders by hand, the Benjamini-Hochberg adjustment of edgeR's p values by hand.
- simpson: Pearson's r by hand on the data the Cards export; the admission rates and the
  Mantel-Haenszel common odds ratio by hand from the counts.

Each test also changes a param, reruns just that Card and checks the result moved as it should.
"""

from __future__ import annotations

import csv
import io
import math
import os
import statistics

import pytest

from conftest import demo_environment
from projects import start_run, wait_run
from test_demos import connect, edit_card, set_param, stderr
from tsugumi_client import path, text
from Tsugumi.Data.DisplayValue import DisplayValue
from Tsugumi.Data.RunState import RunState

pytestmark = pytest.mark.skipif(os.environ.get("TSUGUMI_TEST_DEMO_RUNS") != "1",
                                reason="runs the demo (large Nix closures): TSUGUMI_TEST_DEMO_RUNS=1")


def run(c, cards, *card, propagate=True):
    rid = start_run(c, list(card), propagate=propagate)
    st = wait_run(c, rid, timeout=3000)
    assert st.state == RunState.succeeded, {name: stderr(c, rid, name)[-2000:] for name in cards}
    return rid


def rerun(c, card):
    rid = start_run(c, [card], propagate=False)
    st = wait_run(c, rid, timeout=600)
    assert st.state == RunState.succeeded, stderr(c, rid, card)[-2000:]
    return rid


def read_csv(c, rid, card, member=None):
    """The rows of a CSV a Card kept as its artifact, as dicts of strings."""
    art = next(a for a in c.call("ArtifactListRequest", runId=rid, card=path(card)).artifacts)
    fields = {"member": member} if member else {}
    data, offset = b"", 0
    while True:
        part = c.call("ArtifactReadRequest", artifactId=text(art.artifactId), offset=offset, **fields)
        data += bytes(part.bytes)
        if part.eof:
            return list(csv.DictReader(io.StringIO(data.decode())))
        offset = part.nextOffset


def number(value):
    return float(value) if value not in ("NA", "") else None


def table(c, rid, card):
    d = c.call("RunRecordRequest", runId=rid, card=path(card)).record.display
    assert d.valueType == DisplayValue.DisplayTable, card
    columns = [text(x) for x in d.value.columns]
    return [dict(zip(columns, (text(x) for x in row.cells))) for row in d.value.rows]


def image(c, rid, card):
    d = c.call("RunRecordRequest", runId=rid, card=path(card)).record.display
    assert d.valueType == DisplayValue.DisplayImage, card
    assert bytes(d.value.data)[:4] == b"\x89PNG", card
    assert "missing from font" not in stderr(c, rid, card), card
    return text(d.value.alt)


def kaplan_meier(rows):
    """[(time, survival)] at each death time, by the product-limit formula."""
    times = sorted({r[0] for r in rows if r[1]})
    s, curve = 1.0, []
    for t in times:
        at_risk = sum(1 for r in rows if r[0] >= t)
        deaths = sum(1 for r in rows if r[0] == t and r[1])
        s *= 1 - deaths / at_risk
        curve.append((t, s))
    return curve


def km_median(curve):
    return next(t for t, s in curve if s <= 0.5)


def log_rank(rows):
    """rows: (time, event, group); the chi-square of two groups."""
    o_minus_e, variance = 0.0, 0.0
    for t in sorted({r[0] for r in rows if r[1]}):
        n = sum(1 for r in rows if r[0] >= t)
        n1 = sum(1 for r in rows if r[0] >= t and r[2])
        d = sum(1 for r in rows if r[0] == t and r[1])
        d1 = sum(1 for r in rows if r[0] == t and r[1] and r[2])
        o_minus_e += d1 - d * n1 / n
        if n > 1:
            variance += d * (n1 / n) * (1 - n1 / n) * (n - d) / (n - 1)
    return o_minus_e ** 2 / variance


def cox_one_covariate(rows):
    """rows: (time, event, x). Beta and its standard error, with Efron's ties."""
    times = sorted({r[0] for r in rows if r[1]})
    beta = 0.0
    for _ in range(25):
        score, information = 0.0, 0.0
        for t in times:
            risk = [r for r in rows if r[0] >= t]
            deaths = [r for r in rows if r[0] == t and r[1]]
            s0 = sum(math.exp(beta * r[2]) for r in risk)
            s1 = sum(r[2] * math.exp(beta * r[2]) for r in risk)
            s2 = sum(r[2] ** 2 * math.exp(beta * r[2]) for r in risk)
            d0 = sum(math.exp(beta * r[2]) for r in deaths)
            d1 = sum(r[2] * math.exp(beta * r[2]) for r in deaths)
            d2 = sum(r[2] ** 2 * math.exp(beta * r[2]) for r in deaths)
            score += sum(r[2] for r in deaths)
            for k in range(len(deaths)):
                f = k / len(deaths)
                a0, a1, a2 = s0 - f * d0, s1 - f * d1, s2 - f * d2
                score -= a1 / a0
                information += a2 / a0 - (a1 / a0) ** 2
        beta += score / information
    return beta, math.sqrt(1 / information)


def pearson(xs, ys):
    mx, my = sum(xs) / len(xs), sum(ys) / len(ys)
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    sxx = sum((x - mx) ** 2 for x in xs)
    syy = sum((y - my) ** 2 for y in ys)
    return sxy / math.sqrt(sxx * syy)


# ---------------------------------------------------------------------- survival

SURVIVAL_CARDS = ("load", "cohort", "km", "cox", "zph", "specs")


@pytest.fixture
def survival(tmp_path):
    env = demo_environment(tmp_path, "survival")
    yield env
    env.cleanup()


def test_survival(survival):
    c = connect(survival)
    rid = run(c, SURVIVAL_CARDS, "start")
    lung = read_csv(c, rid, "load")
    assert len(lung) == 228
    # time, event, female
    rows = [(float(r["time"]), r["status"] == "2", r["sex"] == "2") for r in lung]
    assert sum(1 for r in rows if r[1]) == 165

    # Kaplan-Meier by hand, at every death time of each group.
    curves = read_csv(c, rid, "km", "curves.csv")
    for group, female in (("女性", True), ("男性", False)):
        hand = kaplan_meier([r for r in rows if r[2] == female])
        card = {float(r["time"]): float(r["survival"]) for r in curves if r["group"] == group and float(r["deaths"]) > 0}
        assert len(card) == len(hand)
        for t, s in hand:
            assert card[t] == pytest.approx(s, abs=1e-6)

    # Medians and the log-rank statistic: by hand and as R's documentation prints them.
    medians = {r["group"]: r for r in read_csv(c, rid, "km", "medians.csv")}
    assert float(medians["男性"]["median"]) == km_median(kaplan_meier([r for r in rows if not r[2]])) == 270
    assert float(medians["女性"]["median"]) == km_median(kaplan_meier([r for r in rows if r[2]])) == 426
    assert float(medians["男性"]["logrank_chisq"]) == pytest.approx(log_rank(rows), abs=1e-3)
    assert float(medians["男性"]["logrank_chisq"]) == pytest.approx(10.3, abs=0.05)

    # The default model: female, age, ph.ecog, on the 227 patients with a score.
    terms = {r["term"]: r for r in read_csv(c, rid, "cox", "coefficients.csv")}
    assert set(terms) == {"female", "age", "ph.ecog"}
    assert float(terms["female"]["n"]) == 227
    assert float(terms["female"]["coef"]) == pytest.approx(-0.5526, abs=1e-3)
    assert float(terms["ph.ecog"]["coef"]) == pytest.approx(0.4637, abs=1e-3)
    assert "global p 0.2" in image(c, rid, "zph")

    # One specification per pair, and the crude one equal to the Cox model by hand.
    beta, se = cox_one_covariate(rows)
    assert beta == pytest.approx(-0.5310, abs=5e-4)
    assert se == pytest.approx(0.1672, abs=5e-4)
    specs = read_csv(c, rid, "specs")
    assert len(specs) == 24
    crude = next(r for r in specs if r["subset"] == "TRUE" and r["adjustment"] == "調整なし")
    assert float(crude["hr"]) == pytest.approx(math.exp(beta), abs=1e-3)
    older = next(r for r in specs if r["subset"] == "age >= 65" and r["adjustment"] == "調整なし")
    older_rows = [(float(r["time"]), r["status"] == "2", r["sex"] == "2") for r in lung if float(r["age"]) >= 65]
    assert float(older["n"]) == len(older_rows)
    assert float(older["hr"]) == pytest.approx(math.exp(cox_one_covariate(older_rows)[0]), abs=1e-3)
    assert image(c, rid, "specs") and image(c, rid, "km")

    # A param of cox: the crude model gives exactly the hand value.
    edit_card(survival, c, "cox", lambda b: set_param(b, "covariates", ["female"]))
    terms = {r["term"]: r for r in read_csv(c, rerun(c, "cox"), "cox", "coefficients.csv")}
    assert float(terms["female"]["coef"]) == pytest.approx(beta, abs=1e-3)
    assert float(terms["female"]["n"]) == 228

    # A param of cohort: fewer people, as counted by hand.
    edit_card(survival, c, "cohort", lambda b: set_param(b, "age_min", 65))
    assert len(read_csv(c, rerun(c, "cohort"), "cohort")) == len(older_rows)

    # ph.karno breaks proportional hazards, which zph shows once the model has it.
    edit_card(survival, c, "cox", lambda b: set_param(b, "covariates", ["female", "age", "ph.karno"]))
    edit_card(survival, c, "cohort", lambda b: set_param(b, "age_min", 0))
    rid = run(c, SURVIVAL_CARDS, "cohort")  # with what follows it: zph reads the new model
    alt = image(c, rid, "zph")
    assert float(alt.rsplit(" ", 1)[1]) < 0.05, alt


# ------------------------------------------------------------------ meta-analysis

META_CARDS = ("trials", "pool", "funnel", "moderator", "sensitivity")


def inverse_variance(yi, vi, tau2=0.0):
    weights = [1 / (v + tau2) for v in vi]
    estimate = sum(w * y for w, y in zip(weights, yi)) / sum(weights)
    return estimate, math.sqrt(1 / sum(weights))


def dersimonian_laird(yi, vi):
    fixed, _ = inverse_variance(yi, vi)
    weights = [1 / v for v in vi]
    q = sum(w * (y - fixed) ** 2 for w, y in zip(weights, yi))
    c = sum(weights) - sum(w * w for w in weights) / sum(weights)
    tau2 = max(0.0, (q - (len(yi) - 1)) / c)
    return tau2, q


@pytest.fixture
def meta(tmp_path):
    env = demo_environment(tmp_path, "meta-analysis")
    yield env
    env.cleanup()


def test_meta_analysis(meta):
    c = connect(meta)
    rid = run(c, META_CARDS, "start")
    trials = read_csv(c, rid, "trials")
    assert len(trials) == 13

    # The log risk ratio and its variance from the counts of each trial.
    yi, vi = [], []
    for t in trials:
        a, b, c1, d = (float(t[k]) for k in ("tpos", "tneg", "cpos", "cneg"))
        y = math.log((a / (a + b)) / (c1 / (c1 + d)))
        v = 1 / a - 1 / (a + b) + 1 / c1 - 1 / (c1 + d)
        assert float(t["yi"]) == pytest.approx(y, abs=1e-9)
        assert float(t["vi"]) == pytest.approx(v, abs=1e-9)
        yi.append(y)
        vi.append(v)

    pooled = {r["model"]: r for r in read_csv(c, rid, "pool")}
    fixed, fixed_se = inverse_variance(yi, vi)
    assert float(pooled["fixed"]["estimate"]) == pytest.approx(fixed, abs=1e-6)
    assert float(pooled["fixed"]["se"]) == pytest.approx(fixed_se, abs=1e-6)
    tau2, q = dersimonian_laird(yi, vi)
    random_, random_se = inverse_variance(yi, vi, tau2)
    assert float(pooled["random"]["tau2"]) == pytest.approx(tau2, abs=1e-6)
    assert float(pooled["random"]["Q"]) == pytest.approx(q, abs=1e-6)
    assert float(pooled["random"]["estimate"]) == pytest.approx(random_, abs=1e-6)
    assert float(pooled["random"]["se"]) == pytest.approx(random_se, abs=1e-6)
    assert math.exp(random_) < math.exp(fixed) < 1
    assert float(pooled["random"]["I2"]) > 90

    # Leaving out the largest trial (TPT Madras, RR about 1) by hand.
    rows = read_csv(c, rid, "sensitivity")
    madras = next(i for i, t in enumerate(trials) if t["author"] == "TPT Madras")
    y8, v8 = yi[:madras] + yi[madras + 1:], vi[:madras] + vi[madras + 1:]
    left_out = [r for r in rows if r["left_out"] == trials[madras]["trial"]]
    assert float(left_out[0]["rr"]) == pytest.approx(math.exp(inverse_variance(y8, v8)[0]), abs=1e-5)
    tau2_8, _ = dersimonian_laird(y8, v8)
    assert float(left_out[1]["rr"]) == pytest.approx(math.exp(inverse_variance(y8, v8, tau2_8)[0]), abs=1e-5)

    # The published REML values of this data (metafor's documentation; Viechtbauer 2010).
    reml = next(r for r in rows if r["name"] == "REML")
    assert float(reml["tau2"]) == pytest.approx(0.3132, abs=5e-4)
    assert math.log(float(reml["rr"])) == pytest.approx(-0.7145, abs=5e-4)

    # Latitude explains much of the heterogeneity, and the slope is the known one.
    regression = read_csv(c, rid, "moderator")[0]
    assert float(regression["slope"]) == pytest.approx(-0.0291, abs=1e-3)
    assert float(regression["tau2"]) == pytest.approx(0.0764, abs=5e-4)
    assert float(regression["tau2"]) < float(regression["tau2_without"])
    for card in ("pool", "funnel", "moderator", "sensitivity"):
        assert image(c, rid, card)

    # Excluding Madras on pool only: the fixed effect RR falls towards the random effect one.
    edit_card(meta, c, "pool", lambda b: set_param(b, "exclude", [int(trials[madras]["trial"])]))
    after = {r["model"]: r for r in read_csv(c, rerun(c, "pool"), "pool")}
    assert float(after["fixed"]["k"]) == 12
    assert float(after["fixed"]["estimate"]) == pytest.approx(inverse_variance(y8, v8)[0], abs=1e-6)
    assert float(after["fixed"]["rr"]) < float(pooled["fixed"]["rr"]) - 0.1

    # Another estimator on pool: REML gives the published tau^2.
    edit_card(meta, c, "pool", lambda b: (set_param(b, "exclude", []), set_param(b, "method", "REML")))
    after = {r["model"]: r for r in read_csv(c, rerun(c, "pool"), "pool")}
    assert float(after["random"]["tau2"]) == pytest.approx(0.3132, abs=5e-4)
    assert float(after["fixed"]["estimate"]) == pytest.approx(fixed, abs=1e-6)


# ------------------------------------------------------------------------ rna-seq

RNA_CARDS = ("load", "filter", "normalize", "pca", "deseq", "volcano", "thresholds", "top", "edger", "compare", "design")


@pytest.fixture
def rna(tmp_path):
    env = demo_environment(tmp_path, "rna-seq")
    yield env
    env.cleanup()


def benjamini_hochberg(ps):
    """Adjusted p values, None staying None."""
    known = sorted((p, i) for i, p in enumerate(ps) if p is not None)
    adjusted = [None] * len(ps)
    smallest = 1.0
    for rank in range(len(known), 0, -1):
        p, i = known[rank - 1]
        smallest = min(smallest, p * len(known) / rank)
        adjusted[i] = smallest
    return adjusted


def test_rna_seq(rna):
    c = connect(rna)
    rid = run(c, RNA_CARDS, "start")
    samples = read_csv(c, rid, "filter", "samples.csv")
    assert len(samples) == 8
    names = [s["sample"] for s in samples]
    counts = read_csv(c, rid, "filter", "counts.csv")
    assert all(sum(int(g[n]) for n in names) >= 10 for g in counts)

    # Size factors by hand: the median over genes of the ratio to the geometric mean.
    positive = [g for g in counts if all(int(g[n]) > 0 for n in names)]
    logs = [[math.log(int(g[n])) for n in names] for g in positive]
    means = [sum(row) / len(row) for row in logs]
    hand = {n: math.exp(statistics.median(row[j] - m for row, m in zip(logs, means))) for j, n in enumerate(names)}
    size = read_csv(c, rid, "normalize", "size_factors.csv")
    for s in size:
        assert float(s["size_factor"]) == pytest.approx(hand[s["sample"]], rel=1e-6)

    # PC1 separates treated from untreated samples in every cell line.
    pca = read_csv(c, rid, "pca")
    sides = {p["dex"]: set() for p in pca}
    for p in pca:
        sides[p["dex"]].add(float(p["PC1"]) > 0)
    assert len(sides["trt"]) == len(sides["untrt"]) == 1 and sides["trt"] != sides["untrt"]

    # Known glucocorticoid responders go up, as the mean of the log ratios within each cell line by hand.
    results = read_csv(c, rid, "deseq")
    by_symbol = {}
    for r in results:
        if r["symbol"] not in by_symbol or number(r["base_mean"]) > number(by_symbol[r["symbol"]]["base_mean"]):
            by_symbol[r["symbol"]] = r
    genes = {g["gene_id"]: g for g in counts}
    cell = {s["sample"]: s["cell"] for s in samples}
    treated = {s["cell"]: s["sample"] for s in samples if s["dex"] == "trt"}
    untreated = {s["cell"]: s["sample"] for s in samples if s["dex"] == "untrt"}
    assert set(cell.values()) == set(treated) == set(untreated)
    for symbol in ("FKBP5", "DUSP1", "TSC22D3", "KLF15", "PER1", "CRISPLD2"):
        r = by_symbol[symbol]
        assert number(r["fdr"]) < 1e-10, symbol
        assert number(r["log2fc"]) > 1.5, symbol
        g = genes[r["gene_id"]]
        hand_lfc = statistics.mean(math.log2((int(g[treated[k]]) / hand[treated[k]] + 0.5) / (int(g[untreated[k]]) / hand[untreated[k]] + 0.5))
                                   for k in treated)
        assert number(r["log2fc"]) == pytest.approx(hand_lfc, abs=0.35), symbol

    # The threshold table agrees with counting the CSV by hand.
    grid = {r["FDR"]: r for r in table(c, rid, "thresholds")}
    usable = [r for r in results if number(r["fdr"]) is not None]
    for fdr, lfc in ((0.05, 0.0), (0.05, 1.0), (0.001, 2.0)):
        n = sum(1 for r in usable if number(r["fdr"]) < fdr and abs(number(r["log2fc"])) >= lfc)
        assert int(grid[f"< {fdr:g}"][f"|log2FC| ≥ {lfc:g}"]) == n
    shown = {r["遺伝子"]: r for r in table(c, rid, "top")}
    assert {"FKBP5", "DUSP1"} <= set(shown)

    # edgeR's adjustment by hand, and two methods that mostly agree.
    edger = read_csv(c, rid, "edger")
    adjusted = benjamini_hochberg([number(r["p"]) for r in edger])
    for r, a in list(zip(edger, adjusted))[:2000]:
        assert number(r["fdr"]) == pytest.approx(a, rel=1e-6, abs=1e-12)
    in_deseq = {r["gene_id"] for r in usable if number(r["fdr"]) < 0.05}
    in_edger = {r["gene_id"] for r in edger if number(r["fdr"]) < 0.05}
    agreement = {r["Var1"]: int(r["genes"]) for r in read_csv(c, rid, "compare")}
    assert agreement["両方"] == len(in_deseq & in_edger)
    assert agreement["両方"] > 0.6 * len(in_deseq | in_edger)

    # With the cell line in the model many more genes pass, at nearly the same estimates.
    design = read_csv(c, rid, "design")
    paired = sum(1 for r in design if (number(r["fdr_paired"]) or 1) < 0.05)
    unpaired = sum(1 for r in design if (number(r["fdr_unpaired"]) or 1) < 0.05)
    assert paired == len(in_deseq)
    assert paired > 1.3 * unpaired
    both = [r for r in design if number(r["lfc_paired"]) is not None and number(r["lfc_unpaired"]) is not None]
    assert pearson([number(r["lfc_paired"]) for r in both], [number(r["lfc_unpaired"]) for r in both]) > 0.95
    for card in ("filter", "normalize", "pca", "deseq", "volcano", "compare", "design"):
        assert image(c, rid, card)

    # Param of volcano: a stricter fold change leaves the count from the CSV by hand.
    edit_card(rna, c, "volcano", lambda b: set_param(b, "lfc", 2.0))
    alt = image(c, rerun(c, "volcano"), "volcano")
    up = sum(1 for r in usable if number(r["fdr"]) < 0.05 and number(r["log2fc"]) >= 2)
    down = sum(1 for r in usable if number(r["fdr"]) < 0.05 and number(r["log2fc"]) <= -2)
    assert alt == f"Volcano plot: {up} up, {down} down"

    # Param of deseq: dropping the cell line gives the unpaired count of `design`.
    edit_card(rna, c, "deseq", lambda b: set_param(b, "paired", False))
    after = read_csv(c, rerun(c, "deseq"), "deseq")
    assert sum(1 for r in after if (number(r["fdr"]) or 1) < 0.05) == unpaired

    # Param of filter: a stricter threshold keeps fewer genes.
    edit_card(rna, c, "filter", lambda b: set_param(b, "min_total", 100))
    strict = read_csv(c, rerun(c, "filter"), "filter", "counts.csv")
    assert len(strict) == sum(1 for g in counts if sum(int(g[n]) for n in names) >= 100)


# ------------------------------------------------------------------------ simpson

SIMPSON_CARDS = ("penguins", "correlate", "figure", "survey", "admissions", "applications")


@pytest.fixture
def simpson(tmp_path):
    env = demo_environment(tmp_path, "simpson")
    yield env
    env.cleanup()


def strata_by_hand(rows, by):
    groups = {}
    for r in rows:
        if r[by] != "NA":
            groups.setdefault(r[by], []).append(r)
    values = {name: (len(g), pearson([float(r["bill_length_mm"]) for r in g], [float(r["bill_depth_mm"]) for r in g]))
              for name, g in groups.items()}
    usable = [r for g in groups.values() for r in g]
    overall = pearson([float(r["bill_length_mm"]) for r in usable], [float(r["bill_depth_mm"]) for r in usable])
    return overall, values


def check_correlations(rows, card_rows, by):
    overall, values = strata_by_hand(rows, by)
    by_name = {r["stratum"]: r for r in card_rows}
    assert float(by_name["全体"]["r"]) == pytest.approx(overall, abs=1e-9)
    for name, (n, r) in values.items():
        assert float(by_name[name]["n"]) == n
        assert float(by_name[name]["r"]) == pytest.approx(r, abs=1e-9)
        assert (by_name[name]["reversed"] == "TRUE") == (r * overall < 0)
    return overall, values


def test_simpson(simpson):
    c = connect(simpson)
    rid = run(c, SIMPSON_CARDS, "start")
    penguins = read_csv(c, rid, "penguins")
    assert len(penguins) == 342

    # By species: negative overall, positive in every species (Gorman et al. 2014's data).
    overall, values = check_correlations(penguins, read_csv(c, rid, "correlate"), "species")
    assert overall < -0.2
    assert all(r > 0.3 for _, r in values.values())
    for card in ("figure", "survey", "applications"):
        assert image(c, rid, card)

    # Berkeley, 1973: the rates and the odds ratios by hand from the counts.
    rows = read_csv(c, rid, "admissions")
    count = {(r["admit"], r["gender"], r["dept"]): int(r["count"]) for r in rows}
    assert sum(count.values()) == 4526
    depts = sorted({k[2] for k in count})
    m_adm = sum(count["Admitted", "Male", d] for d in depts)
    m_rej = sum(count["Rejected", "Male", d] for d in depts)
    f_adm = sum(count["Admitted", "Female", d] for d in depts)
    f_rej = sum(count["Rejected", "Female", d] for d in depts)
    crude = m_adm * f_rej / (m_rej * f_adm)
    numerator, denominator = 0.0, 0.0
    for d in depts:
        a, b = count["Admitted", "Male", d], count["Rejected", "Male", d]
        c1, e = count["Admitted", "Female", d], count["Rejected", "Female", d]
        n = a + b + c1 + e
        numerator += a * e / n
        denominator += b * c1 / n
    common = numerator / denominator
    assert common == pytest.approx(0.9047, abs=1e-4)
    shown = {r["学科"]: r for r in table(c, rid, "admissions")}
    assert shown["全体 (集計)"]["オッズ比"] == f"{crude:.2f}" == "1.84"
    assert shown["全体 (集計)"]["男性合格率"] == f"{100 * m_adm / (m_adm + m_rej):.1f}%"
    assert shown["全体 (集計)"]["女性合格率"] == f"{100 * f_adm / (f_adm + f_rej):.1f}%"
    assert shown["学科で調整 (Mantel–Haenszel)"]["オッズ比"].startswith(f"{common:.2f} ")
    assert crude > 1 > common  # the reversal

    # Stratifying by sex does not reverse it, by island only in part.
    edit_card(simpson, c, "correlate", lambda b: set_param(b, "stratify", "sex"))
    _, values = check_correlations(penguins, read_csv(c, rerun(c, "correlate"), "correlate"), "sex")
    assert all(r < 0 for _, r in values.values())
    edit_card(simpson, c, "correlate", lambda b: set_param(b, "stratify", "island"))
    _, values = check_correlations(penguins, read_csv(c, rerun(c, "correlate"), "correlate"), "island")
    assert values["Biscoe"][1] < 0 < values["Dream"][1]
