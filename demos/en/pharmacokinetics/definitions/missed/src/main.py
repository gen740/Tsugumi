"""What skipping doses does to each regimen, by the closed-form superposition
that the check Card compared with the numerical solution."""
import json
import math

import matplotlib.pyplot as plt
import numpy as np

from tsugumi import Failure


def concentration(t, dose_times, dose, ke, ka, volume, fraction):
    amplitude = fraction * dose * ka / (volume * (ka - ke))
    total = np.zeros_like(t)
    for start in dose_times:
        s = np.clip(t - start, 0, None)
        total += amplitude * (np.exp(-ke * s) - np.exp(-ka * s))
    return total


def main(inputs, report):
    c = inputs.curves
    params = inputs.params
    missed_at, count = params.missedAtH, int(params.missedCount)
    doses, intervals = c.dosesMg, c.intervalsH
    ke = math.log(2) / c.halfLifeH
    ka, volume, fraction = c.kaPerH, c.volumeL, c.bioavailability
    if abs(ka - ke) < 1e-9:
        raise Failure("output_invalid", "ka equals ke: the closed form here divides by ka - ke")
    low, high = c.lowMgL, c.highMgL
    t = c.tH
    step = t[1] - t[0]

    n = len(doses)
    fig, axes = plt.subplots((n + 1) // 2, 2 if n > 1 else 1, figsize=(11, 3.1 * ((n + 1) // 2) + 0.8), sharex=True, sharey=True,
                             squeeze=False, constrained_layout=True)
    rows = []
    for k, (dose, tau) in enumerate(zip(doses, intervals)):
        ax = axes[k // 2][k % 2]
        times = np.arange(0, t[-1], tau)
        skipped = times[times >= missed_at - 1e-9][:count]
        kept = np.array([x for x in times if x not in skipped])
        usual = concentration(t, times, dose, ke, ka, volume, fraction)
        with_gap = concentration(t, kept, dose, ke, ka, volume, fraction)

        after = t >= missed_at
        below_usual = float((usual[after] < low).sum() * step)
        below_gap = float((with_gap[after] < low).sum() * step)
        lowest = float(with_gap[after].min())
        # Back within 5% of the usual curve, counted from the first dose skipped.
        back = None
        if len(skipped):
            differs = np.nonzero(np.abs(with_gap - usual) > 0.05 * usual)[0]
            differs = differs[t[differs] >= skipped[0]]
            back = float(t[differs[-1]] - skipped[0]) if len(differs) and differs[-1] < len(t) - 1 else None

        ax.axhspan(low, high, color="#3a9a4a", alpha=0.14, linewidth=0)
        ax.plot(t, usual, color="0.65", linewidth=1.1, label="as usual")
        ax.plot(t, with_gap, color="#d9622b", linewidth=1.5, label=f"{len(skipped)} missed")
        for x in skipped:
            ax.axvline(x, color="#d9622b", linewidth=0.8, linestyle=":")
        ax.set_title(f"{dose:g} mg / {tau:g} h: lowest {lowest:.2f} mg/L, below the range {below_usual:.0f} → {below_gap:.0f} hours"
                     + ("" if back is None else f", {back:.0f} hours to recover"), fontsize=10)
        ax.grid(True, alpha=0.25)
        rows.append({"dose": float(dose), "tau": float(tau), "skipped": [float(x) for x in skipped], "lowest": lowest,
                     "below_usual_h": below_usual, "below_missed_h": below_gap, "back_h": back})
    axes[0][0].set_ylim(top=axes[0][0].get_ylim()[1] * 1.22)  # room for the legend above the peaks
    axes[0][0].legend(fontsize=9, loc="upper right", ncol=2)
    if n % 2 and n > 1:
        axes[-1][-1].set_visible(False)
    for row in axes:
        row[0].set_ylabel("Concentration (mg/L)")
    for ax in axes[-1]:
        ax.set_xlabel("Time (h)")
    fig.suptitle(f"Missing {count} doses from hour {missed_at:g} (a made-up drug. Green is an invented therapeutic range, not dosing guidance)", fontsize=12)
    report.figure()
    report.log("result: " + json.dumps(rows))
