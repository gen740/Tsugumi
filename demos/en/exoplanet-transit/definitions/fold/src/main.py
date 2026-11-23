from card import Artifact, Input, Output, params as card_params
import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np


def png(fig):
    import io
    buf = io.BytesIO()
    fig.savefig(buf, format="png", dpi=110, bbox_inches="tight")
    plt.close(fig)
    return buf.getvalue()


def run(inputs: Input) -> Output:
    artifact, directory = Artifact.dir()
    p = card_params
    d = np.load(os.path.join(inputs.require("detrended").open(), "detrended.npz"))
    t, f, period, t0, dur = d["t"], d["flux"], float(d["period"]), float(d["t0"]), float(d["duration"])
    phase = ((t - t0 + period / 2) % period) - period / 2
    hours = phase * 24
    inside = np.abs(phase) < dur * p.core_fraction / 2
    outside = np.abs(phase) > dur
    depth = np.median(f[outside]) - np.median(f[inside])
    ratio = np.sqrt(max(depth, 0))
    bins = np.linspace(-p.window_hours, p.window_hours, 61)
    k = np.digitize(hours, bins)
    centers = (bins[:-1] + bins[1:]) / 2
    binned = np.array([np.median(f[k == i]) if np.any(k == i) else np.nan for i in range(1, len(bins))])
    fig, ax = plt.subplots(figsize=(7, 4))
    ax.plot(hours, f, ".", ms=2, alpha=0.3, color="#888")
    ax.plot(centers, binned, "o-", ms=3, color="#E5484D")
    ax.set_xlim(-p.window_hours, p.window_hours)
    ax.set_xlabel("hours from mid-transit")
    ax.set_ylabel("relative flux")
    ax.set_title(f"Kepler-6 b folded at P = {period:.4f} d")
    with open(os.path.join(directory, "folded.png"), "wb") as fh:
        fh.write(png(fig))
    print(f"depth {depth * 1e6:.0f} ppm, Rp/R* {ratio:.4f}")
    # TODO(open): Port the demo display.
    return Output(folded=artifact)
