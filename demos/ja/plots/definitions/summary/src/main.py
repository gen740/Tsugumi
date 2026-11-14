"""Plain matplotlib, as written for a window: report.figure() puts the figure in the
Card's display (Tsugumi's backend, chosen for every Python Card)."""
import matplotlib.pyplot as plt
import numpy as np


def main(inputs, report):
    w = inputs.walks
    paths = w.positions.reshape(w.walkers, w.steps + 1)
    t = np.arange(w.steps + 1)

    fig, (left, right) = plt.subplots(1, 2, figsize=(9, 3.6), gridspec_kw={"width_ratios": [3, 2]})
    left.plot(t, paths[:40].T, lw=0.7, alpha=0.6)
    sigma = np.sqrt(t)
    left.fill_between(t, -2 * sigma, 2 * sigma, color="tab:blue", alpha=0.12, label=r"$\pm 2\sqrt{t}$")
    left.set(xlabel="step", ylabel="position", title=f"40 of {w.walkers} random walks")
    left.legend(loc="upper left")

    final = paths[:, -1]
    # Final positions have the parity of the step count: bins of two sites each.
    bins = np.arange(final.min() - 1, final.max() + 4, 4)
    right.hist(final, bins=bins, density=True, color="tab:orange", alpha=0.75)
    x = np.linspace(final.min(), final.max(), 200)
    n = w.steps
    right.plot(x, np.exp(-x**2 / (2 * n)) / np.sqrt(2 * np.pi * n), "k--", label=r"$\mathcal{N}(0, t)$")
    right.set(xlabel=f"position after {n} steps", title="where they end up")
    right.legend()
    fig.suptitle("Random walks spread as √t（拡散は √t で広がる）")
    fig.tight_layout()
    report.figure()
