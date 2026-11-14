"""An animation as it would play in a window: FuncAnimation and report.figure().
The Card's display shows it live; report.figure() returns when it has played."""
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.animation import FuncAnimation


def main(inputs, report):
    w = inputs.walks
    paths = w.positions.reshape(w.walkers, w.steps + 1)
    n = w.steps
    frames = np.unique(np.linspace(1, n, inputs.params.frames).astype(int))
    lim = 3.5 * np.sqrt(n)

    fig, (left, right) = plt.subplots(1, 2, figsize=(9, 3.6))
    lines = left.plot(np.zeros((1, 30)), np.zeros((1, 30)), lw=0.8, alpha=0.7)
    left.set(xlim=(0, n), ylim=(-lim, lim), xlabel="step", ylabel="position")
    upper, = left.plot([], [], "k--", lw=1, label=r"$\pm 2\sqrt{t}$")
    lower, = left.plot([], [], "k--", lw=1)
    left.legend(loc="upper left")
    x = np.linspace(-lim, lim, 300)
    gauss, = right.plot([], [], "k--", lw=1.2, label=r"$2\,\mathcal{N}(0, t)$ per site")
    right.set(xlim=(-lim, lim), xlabel="position", ylabel="share of walkers")
    right.legend(loc="upper right")
    bars = None

    def draw(t):
        nonlocal bars
        for k, line in enumerate(lines):
            line.set_data(np.arange(t + 1), paths[k, : t + 1])
        tt = np.arange(t + 1)
        upper.set_data(tt, 2 * np.sqrt(tt))
        lower.set_data(tt, -2 * np.sqrt(tt))
        # After t steps a walker is on a site of t's parity, two apart: each site
        # holds about twice the normal density.
        sites, counts = np.unique(paths[:, t], return_counts=True)
        if bars is not None:
            bars.remove()
        bars = right.bar(sites, counts / len(paths), width=1.8, color="tab:orange", alpha=0.75)
        expected = 2 * np.exp(-x**2 / (2 * t)) / np.sqrt(2 * np.pi * t)
        gauss.set_data(x, expected)
        right.set_ylim(0, 1.2 * max(counts.max() / len(paths), expected.max()))
        left.set_title(f"step {t} of {n}")
        right.set_title(f"spread {paths[:, t].std():.1f} ≈ √{t} = {np.sqrt(t):.1f}")
        return [*lines, upper, lower, gauss]

    draw(frames[0])  # titles in place before the layout is fitted
    fig.tight_layout()
    # Keep the animation alive while the report plays it.
    _anim = FuncAnimation(fig, draw, frames=frames, interval=60, repeat=False)
    report.figure()
