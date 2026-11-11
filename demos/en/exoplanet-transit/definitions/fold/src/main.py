import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from inputs.detrended.exo.DetrendedDir import DetrendedDir
import outputs.folded.exo.FoldedDir as out


def emit(ctx, port, out):
    b = ctx.builder()
    s = b.CreateString(ctx.output_directory)
    out.Start(b)
    out.AddValue(b, s)
    ctx.output(port, b, out.End(b))


def png(fig):
    import io
    buf = io.BytesIO()
    fig.savefig(buf, format="png", dpi=110, bbox_inches="tight")
    plt.close(fig)
    return buf.getvalue()


def main(ctx):
    p = ctx.params()
    d = np.load(os.path.join(ctx.input("detrended", DetrendedDir).Value().decode(), "detrended.npz"))
    t, f, period, t0, dur = d["t"], d["flux"], float(d["period"]), float(d["t0"]), float(d["duration"])
    phase = ((t - t0 + period / 2) % period) - period / 2
    hours = phase * 24
    inside = np.abs(phase) < dur * p.CoreFraction() / 2
    outside = np.abs(phase) > dur
    depth = np.median(f[outside]) - np.median(f[inside])
    ratio = np.sqrt(max(depth, 0))
    bins = np.linspace(-p.WindowHours(), p.WindowHours(), 61)
    k = np.digitize(hours, bins)
    centers = (bins[:-1] + bins[1:]) / 2
    binned = np.array([np.median(f[k == i]) if np.any(k == i) else np.nan for i in range(1, len(bins))])
    fig, ax = plt.subplots(figsize=(7, 4))
    ax.plot(hours, f, ".", ms=2, alpha=0.3, color="#888")
    ax.plot(centers, binned, "o-", ms=3, color="#E5484D")
    ax.set_xlim(-p.WindowHours(), p.WindowHours())
    ax.set_xlabel("hours from mid-transit")
    ax.set_ylabel("relative flux")
    ax.set_title(f"Kepler-6 b folded at P = {period:.4f} d")
    with open(os.path.join(ctx.output_directory, "folded.png"), "wb") as fh:
        fh.write(png(fig))
    print(f"depth {depth * 1e6:.0f} ppm, Rp/R* {ratio:.4f}")
    ctx.display_markdown(
        "**Kepler-6 b** (folded by phase)\n\n"
        f"Period $P = {period:.4f}$ d, BLS box width $\\tau \\approx {dur * 24:.1f}$ h\n\n"
        f"Depth of the dip $\\delta = 1 - F_\\mathrm{{in}}/F_\\mathrm{{out}} = {depth * 1e6:.0f}$ ppm\n\n"
        "$$\\frac{R_p}{R_\\star} = \\sqrt{\\delta}"
        f" = {ratio:.4f}$$\n\n"
        "(An approximation that ignores limb darkening and the shape of the planet's overlap. The folded figure is kept in `folded.png`)"
    )
    emit(ctx, "folded", out)
