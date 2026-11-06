"""Relative potency of the second compound against the first, from the bootstrap samples."""
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.ticker import NullLocator

from inputs.intervals.dose.Intervals import Intervals

COLORS = ["#2a78d6", "#eb6834"]


def main(ctx):
    r = ctx.input("intervals", Intervals)
    n = r.NamesLength()
    names = [r.Names(i).decode() for i in range(n)]
    log_samples = r.LogEc50SamplesAsNumpy().reshape(n, -1)
    hill_samples = r.HillSamplesAsNumpy().reshape(n, -1)

    # Compounds were resampled independently, so paired draws are independent draws.
    log_ratio = log_samples[1] - log_samples[0]
    low, high = 10 ** np.nanpercentile(log_ratio, [2.5, 97.5])
    ratio = r.Ec50(1) / r.Ec50(0)
    true_ratio = r.Ec50True(1) / r.Ec50True(0)
    hill_low, hill_high = np.nanpercentile(hill_samples[1] - hill_samples[0], [2.5, 97.5])
    parallel = hill_low <= 0 <= hill_high
    print(f"relative potency {names[1]}/{names[0]} {ratio:.4g} [{low:.4g}, {high:.4g}] true {true_ratio:g}; "
          f"hill difference [{hill_low:.3f}, {hill_high:.3f}]")

    fig, (left, right) = plt.subplots(1, 2, figsize=(9, 4), gridspec_kw={"width_ratios": [1.1, 1]})
    for i in range(n):
        y = n - 1 - i
        left.plot([r.Ec50Low(i), r.Ec50High(i)], [y, y], color=COLORS[i], lw=3, solid_capstyle="round")
        left.plot(r.Ec50(i), y, "o", ms=8, color=COLORS[i], mec="white", label="推定と 95% 区間" if i == 0 else None)
        left.plot(r.Ec50True(i), y, "D", ms=6, mfc="none", mec="#333333", label="真の値" if i == 0 else None)
    left.axvspan(r.ConcLow(), r.ConcHigh(), color="#000000", alpha=0.05, label="測定範囲")
    left.set_xscale("log")
    shown = [v for i in range(n) for v in (r.Ec50Low(i), r.Ec50High(i), r.Ec50True(i))]
    left.set_xlim(min(shown) / 1.5, max(shown) * 1.5)
    ticks = np.geomspace(min(shown) / 1.2, max(shown) * 1.2, 4)
    left.set_xticks(ticks, [f"{t:.2g}" for t in ticks])
    left.xaxis.set_minor_locator(NullLocator())
    left.set_yticks(range(n), names[::-1])
    left.set_ylim(-0.7, n - 0.3)
    left.set_xlabel("EC50 (µM)")
    left.set_title("EC50（合成データ）")
    left.legend(frameon=False, loc="upper center", ncol=3, fontsize=8, bbox_to_anchor=(0.5, -0.2))
    left.spines[["top", "right"]].set_visible(False)

    finite = log_ratio[np.isfinite(log_ratio)]
    right.hist(10 ** finite, bins=np.geomspace(10 ** finite.min(), 10 ** finite.max(), 40), color="#888888", alpha=0.7)
    right.axvline(true_ratio, color="#333333", lw=1.5, ls="--", label=f"真の比 {true_ratio:.3g}")
    right.axvspan(low, high, color="#2a78d6", alpha=0.12, label=f"95% 区間 {low:.3g} – {high:.3g}")
    right.set_xscale("log")
    ticks = np.geomspace(10 ** finite.min(), 10 ** finite.max(), 5)
    right.set_xticks(ticks, [f"{t:.3g}" for t in ticks])
    right.xaxis.set_minor_locator(NullLocator())
    right.set_xlabel(f"相対力価 EC50({names[1]}) / EC50({names[0]})")
    right.set_ylabel(f"ブートストラップ {r.NBoot()} 回の度数")
    if low <= 1 <= high:
        right.set_title(f"力価の差は言えない（比 {ratio:.3g}、区間が 1 を含む）")
    elif ratio > 1:
        right.set_title(f"{names[0]} は {names[1]} の {ratio:.3g} 倍強い")
    else:
        right.set_title(f"{names[1]} は {names[0]} の {1 / ratio:.3g} 倍強い")
    right.legend(frameon=False, loc="upper center", ncol=2, fontsize=8, bbox_to_anchor=(0.5, -0.2))
    right.spines[["top", "right"]].set_visible(False)

    if parallel:
        text = f"Hill 勾配の差の区間 {hill_low:.2f} – {hill_high:.2f} は 0 を含み、曲線が平行でないとは言えません。"
    else:
        text = f"Hill 勾配の差の区間 {hill_low:.2f} – {hill_high:.2f} は 0 を含みません。曲線は平行でなく、この比は EC50 の水準でだけ言えます。"
    fig.text(0.5, 0.01, text, ha="center", fontsize=9)
    fig.tight_layout(rect=(0, 0.05, 1, 1))
    plt.show()
