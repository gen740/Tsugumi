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
    left.axhspan(low, high, color="#3a9a4a", alpha=0.14, linewidth=0, label=f"有効域 {low:g}–{high:g} mg/L（作り物）")
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
        left.plot(since, y, color=color, linewidth=1.8, label=f"{name}: 山 {peak:.2f}、谷 {trough:.2f}")
        right.barh(k, 100 * below, color="#c9d6e6")
        right.barh(k, 100 * inside, left=100 * below, color="#3a9a4a", alpha=0.75)
        right.barh(k, 100 * above, left=100 * (below + inside), color="#e0a07e")
        right.text(50, k, f"域内 {100 * inside:.0f}%", ha="center", va="center", fontsize=10)
        rows.append({"dose": float(dose), "tau": float(tau), "peak": peak, "trough": trough, "below": below, "inside": inside, "above": above})

    left.set_xlabel("最後の投与間隔での投与後の時間 (h)")
    left.set_ylabel("濃度 (mg/L)")
    left.set_ylim(bottom=0)
    left.set_title("定常状態の山と谷（架空の薬物）", fontsize=12)
    left.grid(True, alpha=0.25)
    left.legend(fontsize=9, loc="upper right")
    right.set_yticks(range(len(names)), names)
    right.invert_yaxis()
    right.set_xlim(0, 100)
    right.set_xlabel("間隔のうちの時間 (%): 下（青）、域内（緑）、上（橙）")
    right.set_title("有効域の中にいる時間", fontsize=12)
    report.figure()
    report.log(str("result: " + json.dumps(rows)))
