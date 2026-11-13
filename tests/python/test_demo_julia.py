"""The four Julia demos, run whole against references computed here, not by the Cards' code.

battery: the DP's saving equals a forward DP written here, its schedule keeps the battery's limits and reproduces its
bill, a flat tariff with equal selling price leaves the battery still, and savings saturate with capacity.
gillespie: the stationary mean, variance and Fano factor against the Poisson closed form, the mean of the runs against
the rate equation, CV against 1/sqrt(N), A + B <-> C against the exact distribution.
heat-inverse: the forward model against explicit time steps here and the analytic mode decay, heat conserved, naive
inversion blowing up, recovery without noise at an early time, and a later measurement losing detail.
ecology-models: the 21-year table against a copy here, the LV fit against the Stan case study's posterior, its error
reproduced by an RK4 written here, which also keeps Lotka-Volterra's conserved quantity.

Each also changes a param, reruns and checks that the result moved the right way.
"""

from __future__ import annotations

import filecmp
import json
import math
import os

import pytest

from conftest import ROOT, demo_environment
from projects import start_run, wait_run
from test_demos import connect, edit_card, set_param, stderr
from tsugumi_client import path, text
from Tsugumi.Data.RunState import RunState

pytestmark = pytest.mark.skipif(os.environ.get("TSUGUMI_TEST_DEMO_RUNS") != "1",
                                reason="runs the demo (large Nix closures): TSUGUMI_TEST_DEMO_RUNS=1")


@pytest.fixture
def demo(request, tmp_path):
    env = demo_environment(tmp_path, request.param)
    yield env
    env.cleanup()


def run(c, card="start", propagate=True, timeout=900):
    rid = start_run(c, [card], propagate=propagate)
    st = wait_run(c, rid, timeout=timeout)
    assert st.state == RunState.succeeded, (st.failure and text(st.failure.diagnostic))
    return rid


def spec(c, rid, card):
    d = c.call("RunRecordRequest", runId=rid, card=path(card)).record.display
    assert d.valueType == 7 and text(d.value.module).endswith("-tsugumi-view-vega-lite"), stderr(c, rid, card)
    raw = text(d.value.configJson)
    assert len(raw) < 1 << 20
    return json.loads(raw)


def meta(c, rid, card):
    return spec(c, rid, card)["usermeta"]


def table(c, rid, card):
    d = c.call("RunRecordRequest", runId=rid, card=path(card)).record.display
    assert d.valueType == 4, stderr(c, rid, card)
    return [[text(x) for x in r.cells] for r in d.value.rows]


# ---- battery ----------------------------------------------------------------------------

def bill(grid, price, ratio):
    return price * grid if grid > 0 else ratio * price * grid


def forward_dp(net, price, ratio, capacity=10.0, power=3.0, efficiency=0.9, step=0.1):
    """Least bill, walking forward from the half-full start (the Card goes backward)."""
    levels = round(capacity / step)
    start = levels // 2
    way = math.sqrt(efficiency)
    up, down = int(power * way / step + 1e-9), int(power / way / step + 1e-9)
    best = {start: 0.0}
    for n, p in zip(net, price):
        following = {}
        for s, cost in best.items():
            for to in range(max(0, s - down), min(levels, s + up) + 1):
                stored = (to - s) * step
                ac = stored / way if stored > 0 else stored * way
                total = cost + bill(n + ac, p, ratio)
                if total < following.get(to, math.inf):
                    following[to] = total
        best = following
    return best[start]


@pytest.mark.parametrize("demo", ["battery"], indirect=True)
def test_battery(demo):
    c = connect(demo)
    rid = run(c)
    market = meta(c, rid, "market")
    net = [a - b for a, b in zip(market["load"], market["solar"])]
    assert len(net) == 168

    s = meta(c, rid, "schedule")
    no_battery = sum(bill(n, p, 0.5) for n, p in zip(net, market["spot"]))
    assert s["baseline"] == pytest.approx(no_battery)
    assert s["bill"] == pytest.approx(forward_dp(net, market["spot"], 0.5), abs=1e-6)
    assert s["saved"] > 0

    # The schedule itself: limits, returning to the start, its grid flows and its bill.
    charge, grid = s["charge"], s["grid"]
    way = math.sqrt(0.9)
    assert charge[0] == charge[-1] == pytest.approx(5.0)
    assert all(-1e-9 <= q <= 10 + 1e-9 for q in charge)
    for t, g in enumerate(grid):
        stored = charge[t + 1] - charge[t]
        ac = stored / way if stored > 0 else stored * way
        assert abs(ac) <= 3.0 + 1e-9
        assert g == pytest.approx(net[t] + ac)
    assert sum(bill(g, p, 0.5) for g, p in zip(grid, market["spot"])) == pytest.approx(s["bill"])

    # A flat price sold at the same price: cycling only loses energy.
    results = {r["scenario"]: r for r in meta(c, rid, "tariffs")["results"]}
    still = results["定額・売電も同額"]
    assert still["saved"] == pytest.approx(0, abs=1e-9) and still["cycles"] == 0
    assert forward_dp(net, market["flat"], 1.0) == pytest.approx(sum(bill(n, 30.0, 1.0) for n in net), abs=1e-6)
    assert results["定額・売電 50%"]["saved"] > 100  # selling low: keeping the solar surplus pays
    assert results["市場連動・売電 50%"]["saved"] == pytest.approx(s["saved"])

    # Diminishing returns: savings never fall, and the last kWh are worth far less than the first.
    sweep = meta(c, rid, "capacities")
    for saved in sweep["savings"].values():
        assert all(b >= a - 1e-6 for a, b in zip(saved, saved[1:]))
        sizes = sweep["capacities"]
        assert (saved[-1] - saved[-2]) / (sizes[-1] - sizes[-2]) < 0.2 * (saved[1] - saved[0]) / (sizes[1] - sizes[0])

    assert any(row[0].startswith("DP = 全探索") and row[1] == row[2] for row in table(c, rid, "check"))

    # A flat tariff on the schedule Card alone: it cycles less for less.
    edit_card(demo, c, "schedule", lambda b: set_param(b, "tariff", "flat"))
    flat = meta(c, run(c, "schedule", propagate=False), "schedule")
    assert flat["saved"] == pytest.approx(results["定額・売電 50%"]["saved"])
    assert flat["saved"] < s["saved"] and flat["cycles"] < s["cycles"]

    # Worse efficiency from the settings on: every saving falls.
    edit_card(demo, c, "settings", lambda b: set_param(b, "round_trip_efficiency", 0.6))
    lossy = meta(c, run(c, "settings"), "schedule")
    assert lossy["saved"] < flat["saved"]


# ---- gillespie --------------------------------------------------------------------------

def exact_bound_fraction(total, on_rate, off_rate):
    """A + B <-> C from the stationary law P(c) ~ K^c / (c! ((total - c)!)^2), K = on / off."""
    logs = [c * math.log(on_rate / off_rate) - math.lgamma(c + 1) - 2 * math.lgamma(total - c + 1) for c in range(total + 1)]
    top = max(logs)
    weights = [math.exp(v - top) for v in logs]
    return sum(c * w for c, w in enumerate(weights)) / sum(weights) / total


@pytest.mark.parametrize("demo", ["gillespie"], indirect=True)
def test_gillespie(demo):
    c = connect(demo)
    rid = run(c)
    k, gamma, volume, runs = 2.0, 0.1, 1.0, 2000
    lam = k * volume / gamma
    s = meta(c, rid, "stationary")
    assert s["theory"] == pytest.approx(lam)
    assert abs(s["mean"] - lam) < 4 * math.sqrt(lam / runs)
    assert abs(s["variance"] - lam) < 4 * math.sqrt((lam + 2 * lam ** 2) / runs)
    assert abs(s["fano"] - 1) < 4 * math.sqrt(2 / runs)

    t = meta(c, rid, "trajectories")
    for time, average in zip(t["times"][20:], t["average"][20:]):
        expected = lam * (1 - math.exp(-gamma * time))
        assert abs(average - expected) < 4.5 * math.sqrt(expected / runs)

    scaling = meta(c, rid, "scaling")
    for v, cv in zip(scaling["volumes"], scaling["cvs"]):
        assert cv == pytest.approx(1 / math.sqrt(k * v / gamma), rel=0.1)
    assert scaling["cvs"] == sorted(scaling["cvs"], reverse=True)  # each volume noisier than the next larger

    binding = meta(c, rid, "binding")
    for total, ssa, ode, exact in zip(binding["totals"], binding["ssa"], binding["ode"], binding["exact"]):
        assert exact == pytest.approx(exact_bound_fraction(total, 1.0 / (total / 2.0), 1.0))
        assert ssa == pytest.approx(exact, abs=0.012)
        assert ode == pytest.approx(0.5)  # (2 - c)^2 = c
    gaps = [e - o for e, o in zip(binding["exact"], binding["ode"])]
    assert gaps == sorted(gaps, reverse=True) and gaps[0] > 0.05 and gaps[-1] < 0.002

    assert all(row[-1] == "範囲内" for row in table(c, rid, "check"))

    # Smaller volumes on the scaling Card alone: more noise.
    edit_card(demo, c, "scaling", lambda b: set_param(b, "volumes", [0.02, 0.05]))
    small = meta(c, run(c, "scaling", propagate=False), "scaling")
    assert small["cvs"][0] == pytest.approx(math.sqrt(1 / 0.4), rel=0.1) and small["cvs"][0] > scaling["cvs"][0]

    # A tenth of the volume from the settings on: a tenth of the molecules, still Poisson.
    edit_card(demo, c, "settings", lambda b: set_param(b, "volume", 0.1))
    rid = run(c, "settings")
    low = meta(c, rid, "stationary")
    assert low["theory"] == pytest.approx(2.0)
    assert abs(low["mean"] - 2) < 4 * math.sqrt(2 / runs) and abs(low["fano"] - 1) < 0.2
    assert all(row[-1] == "範囲内" for row in table(c, rid, "check"))


# ---- heat-inverse -----------------------------------------------------------------------

def explicit_heat(u, alpha, T):
    """Explicit time steps with insulated ends, small enough to be close to the exact flow."""
    n = len(u)
    h = 1 / n
    steps = max(1, math.ceil(T * alpha / (0.05 * h * h)))
    r = alpha * (T / steps) / (h * h)
    u = list(u)
    for _ in range(steps):
        u = [u[j] + r * ((u[j - 1] if j > 0 else u[j]) - 2 * u[j] + (u[j + 1] if j < n - 1 else u[j])) for j in range(n)]
    return u


@pytest.mark.parametrize("demo", ["heat-inverse"], indirect=True)
def test_heat_inverse(demo):
    c = connect(demo)
    rid = run(c)
    alpha, T, n, noise = 1.0, 0.002, 100, 0.2
    f = meta(c, rid, "forward")
    x, truth, clean, measured = f["x"], f["truth"], f["clean"], f["measured"]

    assert sum(clean) == pytest.approx(sum(truth), rel=1e-12)  # insulated ends keep the heat
    assert max(abs(a - b) for a, b in zip(clean, explicit_heat(truth, alpha, T))) < 0.02
    # A cosine is a mode of the rod: its coefficient decays by the discrete factor, close to the continuous one.
    for k in (1, 3, 7):
        mode = [math.cos(k * math.pi * a) for a in x]
        before = sum(a * b for a, b in zip(truth, mode))
        after = sum(a * b for a, b in zip(clean, mode))
        discrete = math.exp(-alpha * 4 * n * n * math.sin(k * math.pi / (2 * n)) ** 2 * T)
        assert after == pytest.approx(before * discrete, rel=1e-9)
        assert discrete == pytest.approx(math.exp(-alpha * (k * math.pi) ** 2 * T), rel=0.01)
    rms_noise = math.sqrt(sum((a - b) ** 2 for a, b in zip(measured, clean)) / n)
    assert rms_noise == pytest.approx(noise, rel=0.3)

    sigma = meta(c, rid, "spectrum")["singular_values"]
    closed = sorted(math.exp(-alpha * 4 * n * n * math.sin(k * math.pi / (2 * n)) ** 2 * T) for k in range(n))[::-1]
    assert sigma == pytest.approx(closed, rel=1e-6)

    naive = meta(c, rid, "naive")
    assert naive["peak"] > 1e10 * max(truth)  # the blow-up; noise is amplified by up to 1 / sigma_min
    assert naive["peak"] > naive["peak_clean"] > 1e3

    sweep = meta(c, rid, "tikhonov")
    errors = dict(zip(sweep["lambdas"], sweep["errors"]))
    residuals = dict(zip(sweep["lambdas"], sweep["residuals"]))
    assert residuals[sweep["discrepancy"]] <= noise * math.sqrt(n)
    assert errors[sweep["discrepancy"]] < 1.5 * min(errors.values())
    assert min(errors.values()) < 0.01 * errors[sweep["lambdas"][0]]  # regularization beats the near-naive end

    r = meta(c, rid, "reconstruct")
    assert r["lambda"] == sweep["discrepancy"]
    assert r["errors"][1] < r["errors"][0] and r["errors"][1] < r["errors"][2]  # too little and too much are both worse
    rms_truth = math.sqrt(sum((a - b) ** 2 for a, b in zip(r["solution"], truth)) / n)
    assert rms_truth == pytest.approx(r["errors"][1])

    later = meta(c, rid, "horizon")
    assert later["errors"][-1] > 3 * later["errors"][0]  # a later measurement loses detail
    assert later["modes"] == sorted(later["modes"], reverse=True) and later["modes"][-1] < later["modes"][0]

    # Lambda on the reconstruct Card alone: much too large is much worse.
    edit_card(demo, c, "reconstruct", lambda b: set_param(b, "lambda", 1.0))
    wide = meta(c, run(c, "reconstruct", propagate=False), "reconstruct")
    assert wide["lambda"] == 1.0 and wide["errors"][1] > 2 * r["errors"][1]
    edit_card(demo, c, "reconstruct", lambda b: set_param(b, "lambda", 0.0))

    # Measured later from the settings on: the same noise, a worse reconstruction.
    edit_card(demo, c, "settings", lambda b: set_param(b, "final_time", 0.02))
    late = meta(c, run(c, "settings"), "reconstruct")
    assert late["errors"][1] > 1.5 * r["errors"][1]

    # No noise and an early time: the inversion recovers the truth.
    def early(b):
        set_param(b, "final_time", 0.00005)
        set_param(b, "noise", 0.0)
    edit_card(demo, c, "settings", early)
    recovered = meta(c, run(c, "settings"), "reconstruct")
    assert math.sqrt(sum((a - b) ** 2 for a, b in zip(recovered["solution"], truth)) / n) < 1e-3


# ---- ecology-models ---------------------------------------------------------------------

# stan-dev/example-models, knitr/lotka-volterra/hudson-bay-lynx-hare.csv (year, lynx, hare).
PELTS = [(1900, 4.0, 30.0), (1901, 6.1, 47.2), (1902, 9.8, 70.2), (1903, 35.2, 77.4), (1904, 59.4, 36.3), (1905, 41.7, 20.6),
         (1906, 19.0, 18.1), (1907, 13.0, 21.4), (1908, 8.3, 22.0), (1909, 9.1, 25.4), (1910, 7.4, 27.1), (1911, 8.0, 40.3),
         (1912, 12.3, 57.0), (1913, 19.5, 76.6), (1914, 45.7, 52.3), (1915, 51.1, 19.5), (1916, 29.7, 11.2), (1917, 15.8, 7.6),
         (1918, 9.7, 14.6), (1919, 10.1, 16.2), (1920, 8.6, 24.7)]


def lv_rk4(a, b, c, d, H, L, dt):
    def f(H, L):
        return a * H - b * H * L, c * H * L - d * L
    k1 = f(H, L)
    k2 = f(H + dt / 2 * k1[0], L + dt / 2 * k1[1])
    k3 = f(H + dt / 2 * k2[0], L + dt / 2 * k2[1])
    k4 = f(H + dt * k3[0], L + dt * k3[1])
    return H + dt / 6 * (k1[0] + 2 * k2[0] + 2 * k3[0] + k4[0]), L + dt / 6 * (k1[1] + 2 * k2[1] + 2 * k3[1] + k4[1])


@pytest.mark.parametrize("demo", ["ecology-models"], indirect=True)
def test_ecology_models(demo):
    c = connect(demo)
    rid = run(c)
    record = meta(c, rid, "load")
    assert list(zip(record["years"], record["lynx"], record["hare"])) == PELTS

    fits = {m["name"]: m for m in meta(c, rid, "fit")["fits"]}
    lv = fits["ロトカ・ヴォルテラ"]
    p = dict(zip(lv["parameter_names"], lv["parameters"]))
    # Carpenter's Stan case study, the same model and data: 10-90% posterior intervals.
    for name, low, high in (("a", 0.465, 0.630), ("b", 0.022, 0.033), ("c", 0.020, 0.029), ("d", 0.692, 0.926),
                            ("H0", 30.415, 37.630), ("L0", 5.273, 6.614)):
        assert low <= p[name] <= high, (name, p[name])

    # The error and the scores again, by an integrator written here.
    H, L = p["H0"], p["L0"]
    sse = 0.0
    for i, (_, lynx, hare) in enumerate(PELTS):
        if i:
            for _ in range(20):
                H, L = lv_rk4(p["a"], p["b"], p["c"], p["d"], H, L, 0.05)
        sse += (math.log(hare) - math.log(H)) ** 2 + (math.log(lynx) - math.log(L)) ** 2
    assert lv["sse"] == pytest.approx(sse, rel=1e-9)
    n, k = 42, 7
    assert lv["n"] == n and lv["k"] == k
    assert lv["aic"] == pytest.approx(n * math.log(sse / n) + 2 * k, abs=1e-6)
    assert lv["bic"] == pytest.approx(n * math.log(sse / n) + k * math.log(n), abs=1e-6)

    # Lotka-Volterra's conserved quantity along the fitted orbit, for 100 years.
    V = lambda H, L: p["c"] * H - p["d"] * math.log(H) + p["b"] * L - p["a"] * math.log(L)
    H, L = p["H0"], p["L0"]
    start = V(H, L)
    worst = 0.0
    for _ in range(2000):
        H, L = lv_rk4(p["a"], p["b"], p["c"], p["d"], H, L, 0.05)
        worst = max(worst, abs(V(H, L) - start))
    assert worst < 1e-6
    drift = table(c, rid, "check")
    assert float(next(r for r in drift if "dt = 0.05" in r[0])[1]) == pytest.approx(worst, rel=0.05)

    # The models nest: more parameters may not fit worse. Penalized scores favour the simplest here.
    logistic, holling = fits["被食者ロジスティック"], fits["ホリング II 型"]
    assert holling["sse"] <= logistic["sse"] * 1.001 and logistic["sse"] <= lv["sse"] * 1.001
    assert lv["aicc"] < logistic["aicc"] < holling["aicc"]
    assert len(table(c, rid, "scores")) == 3

    forecast = meta(c, rid, "forecast")
    assert forecast["fit_until"] == 1914
    for r in forecast["results"]:
        assert r["rmse_heldout"] > r["rmse_fit"]  # new years are harder than fitted ones

    # Fitting fewer years on the forecast Card alone: a longer forecast is worse.
    edit_card(demo, c, "forecast", lambda b: set_param(b, "fit_until", 1908))
    shorter = meta(c, run(c, "forecast", propagate=False), "forecast")
    assert shorter["fit_until"] == 1908
    assert shorter["results"][0]["rmse_heldout"] > forecast["results"][0]["rmse_heldout"]


def test_the_cards_share_their_source_files():
    """Each Card can be read alone, so the shared code is a copy: the copies stay the same."""
    for demo, name, first, others in (("battery", "dp.jl", "schedule", ["capacities", "tariffs", "check"]),
                                      ("ecology-models", "ecology.jl", "fit", ["forecast", "phase", "check"])):
        base = ROOT / "demos" / "ja" / demo / "definitions"
        for other in others:
            assert filecmp.cmp(base / first / "src" / name, base / other / "src" / name, shallow=False), (demo, other)
