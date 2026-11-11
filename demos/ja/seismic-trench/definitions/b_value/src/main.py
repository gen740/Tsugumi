import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from inputs.section.quake.SectionDir import SectionDir
import outputs.fmd.quake.FmdDir as out


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
    sel = np.loadtxt(os.path.join(ctx.input("section", SectionDir).Value().decode(), "selected.csv"),
                     delimiter=",", skiprows=1, ndmin=2)
    mags = sel[:, 3]
    mc, dm = p.Mc(), p.BinWidth()
    use = mags[mags >= mc - 1e-9]
    # Aki (1965) maximum likelihood with the bin correction of Utsu (1966).
    b = np.log10(np.e) / (use.mean() - (mc - dm / 2))
    b_err = b / np.sqrt(len(use))
    a = np.log10(len(use)) + b * mc
    grid = np.round(np.arange(mags.min(), mags.max() + dm, dm), 2)
    cum = np.array([np.sum(mags >= m - 1e-9) for m in grid])
    ok = cum > 0
    fit = grid >= mc
    with open(os.path.join(ctx.output_directory, "fmd.csv"), "w") as fh:
        fh.write("magnitude,N_ge_M\n" + "".join(f"{m:.1f},{n}\n" for m, n in zip(grid, cum)))
    print(f"{len(use)} events with M >= {mc:g}: b = {b:.2f} +/- {b_err:.2f}, a = {a:.2f}")
    ctx.display_line_chart([
        {"id": "observed", "label": "log10 N(>=M) observed", "x": grid[ok].tolist(), "y": np.log10(cum[ok]).tolist()},
        {"id": "fit", "label": f"Gutenberg-Richter fit b = {b:.2f} +/- {b_err:.2f} (M >= {mc:g})",
         "x": grid[fit].tolist(), "y": (a - b * grid[fit]).tolist()},
    ])
    emit(ctx, "fmd", out)
