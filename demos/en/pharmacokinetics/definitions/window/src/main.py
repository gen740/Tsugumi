"""The last dosing interval of each regimen against the made-up therapeutic
window: peak, trough and the share of the time inside it."""
import json

import matplotlib.pyplot as plt
import numpy as np

from inputs.curves.pk.Curves import Curves

COLORS = ["#2a7ab9", "#d9622b", "#3a9a4a", "#8a5ec2", "#c23b4b", "#7a7a7a"]


def main(inputs, report):
    c = inputs.input("curves", Curves)
    doses, intervals, last_dose = c.DosesMgAsNumpy(), c.IntervalsHAsNumpy(), c.LastDoseHAsNumpy()
    t = c.THAsNumpy()
    conc = c.ConcAsNumpy().reshape(len(doses), len(t))
    low, high = c.LowMgL(), c.HighMgL()

    fig, (left, right) = plt.subplots(1, 2, figsize=(11, 5), width_ratios=[1.7, 1], constrained_layout=True)
    left.axhspan(low, high, color="#3a9a4a", alpha=0.14, linewidth=0, label=f"therapeutic range {low:g}–{high:g} mg/L (invented)")
    rows = []
    names = []
    for k, (dose, tau) in enumerate(zip(doses, intervals)):
        since = np.linspace(0, tau, 2001)
        y = np.interp(last_dose[k] - tau + since, t, conc[k])
        peak, trough = float(y.max()), float(y.min())
        below, above = float((y < low).mean()), float((y > high).mean())
        inside = 1 - below - above
        color = COLORS[k % len(COLORS)]
        name = f"{dose:g} mg / {tau:g} h"
        names.append(name)
        left.plot(since, y, color=color, linewidth=1.8, label=f"{name}: peak {peak:.2f}, trough {trough:.2f}")
        right.barh(k, 100 * below, color="#c9d6e6")
        right.barh(k, 100 * inside, left=100 * below, color="#3a9a4a", alpha=0.75)
        right.barh(k, 100 * above, left=100 * (below + inside), color="#e0a07e")
        right.text(50, k, f"in range {100 * inside:.0f}%", ha="center", va="center", fontsize=10)
        rows.append({"dose": float(dose), "tau": float(tau), "peak": peak, "trough": trough, "below": below, "inside": inside, "above": above})

    left.set_xlabel("Time since the dose in the last interval (h)")
    left.set_ylabel("Concentration (mg/L)")
    left.set_ylim(bottom=0)
    left.set_title("Steady-state peaks and troughs (a made-up drug)", fontsize=12)
    left.grid(True, alpha=0.25)
    left.legend(fontsize=9, loc="upper right")
    right.set_yticks(range(len(names)), names)
    right.invert_yaxis()
    right.set_xlim(0, 100)
    right.set_xlabel("Share of the interval (%): below (blue), in range (green), above (orange)")
    right.set_title("Time inside the therapeutic range", fontsize=12)
    report.figure()
    report.log(str("result: " + json.dumps(rows)))
