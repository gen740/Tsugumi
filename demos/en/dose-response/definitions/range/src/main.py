"""How wide the EC50 interval gets as the top concentration moves: new synthetic
plates with the same design, only the range shifted."""
import json

import matplotlib.pyplot as plt
import numpy as np
from scipy.optimize import curve_fit

from inputs.plate.dose.Plate import Plate

COLORS = ["#2a78d6", "#eb6834"]


def hill4(x, bottom, top, log_ec50, hill):
    return bottom + (top - bottom) / (1 + 10 ** ((log_ec50 - np.log10(x)) * hill))


def start_values(conc, mean):
    low, high = mean.min(), mean.max()
    return [low, high, np.log10(conc[np.argmin(np.abs(mean - (low + high) / 2))]), 1.0]


def interval(conc, reps, y, n_boot, rng):
    """95% interval of the EC50 by the residual bootstrap; None if the fit fails."""
    x = np.repeat(conc, reps)
    bounds = ([-50, -50, np.log10(conc[0]) - 3, 0.1], [150, 400, np.log10(conc[-1]) + 3, 8])
    try:
        est = curve_fit(hill4, x, y, p0=start_values(conc, y.reshape(-1, reps).mean(axis=1)), bounds=bounds, max_nfev=2000)[0]
    except RuntimeError:
        return None
    curve = hill4(x, *est)
    resid = (y - curve) - (y - curve).mean()
    resid = resid * np.sqrt(len(y) / max(len(y) - 4, 1))
    logs = []
    for _ in range(n_boot):
        try:
            logs.append(curve_fit(hill4, x, curve + rng.choice(resid, len(y)), p0=est, bounds=bounds, max_nfev=200)[0][2])
        except RuntimeError:
            pass
    low, high = np.percentile(logs, [2.5, 97.5])
    return 10 ** low, 10 ** high


def main(inputs, report):
    plate = inputs.input("plate", Plate)
    p = inputs.params
    tops = p.topsAsNumpy
    design = plate.ConcAsNumpy() / plate.ConcAsNumpy()[-1]  # the dilution series, relative to its top
    reps = plate.Replicates()
    rng = np.random.default_rng(plate.Seed() + 1)

    fig, ax = plt.subplots(figsize=(7.5, 4.4))
    results = {}
    for i in range(plate.NamesLength()):
        name = plate.Names(i).decode()
        widths = []
        for top in tops:
            conc = top * design
            truth = plate.Bottom(i) + (plate.Top(i) - plate.Bottom(i)) / (1 + (plate.Ec50(i) / conc) ** plate.Hill(i))
            y = (truth[:, None] + rng.normal(0, plate.NoiseSd(), (len(conc), reps))).ravel()
            found = interval(conc, reps, y, p.nBoot, rng)
            widths.append(np.nan if found is None else found[1] / found[0])
        results[name] = [float(w) for w in widths]
        ax.plot(tops, widths, "o-", color=COLORS[i], lw=2, ms=6, label=f"{name} (true EC50 = {plate.Ec50(i):g} µM)")
        ax.axvline(plate.Ec50(i), color=COLORS[i], lw=1, ls=":")
        ax.axvline(plate.Ec50(i) / design[0], color=COLORS[i], lw=1, ls=":")
        ax.axvspan(plate.Ec50(i), plate.Ec50(i) / design[0], color=COLORS[i], alpha=0.07)
    report.log(str("interval widths " + json.dumps({"tops": [float(t) for t in tops], "widths": results})))
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("Highest concentration top (µM)")
    ax.set_ylabel("Width of the EC50's 95% interval (upper ÷ lower)")
    ax.set_title("Bracket the EC50 mid-range and the interval is narrow; miss it and it opens (synthetic data)")
    ax.grid(alpha=0.25)
    ax.spines[["top", "right"]].set_visible(False)
    ax.legend(frameon=False, loc="upper right")
    fig.text(0.5, 0.005, "Coloured band: highest concentrations at which the true EC50 is inside the measured range. The search stops at ± 3 orders of magnitude beyond it, so the width of an open interval means nothing in itself.",
             ha="center", fontsize=8)
    fig.tight_layout(rect=(0, 0.04, 1, 1))
    report.figure()
