import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from astropy.io import fits
from astropy.timeseries import BoxLeastSquares

from inputs.data.exo.LightCurveFiles import LightCurveFiles
import outputs.detrended.exo.DetrendedDir as out


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
    folder = ctx.input("data", LightCurveFiles).Value().decode()
    with fits.open(os.path.join(folder, sorted(os.listdir(folder))[0])) as h:
        d = h[1].data
        t, f, q = np.array(d["TIME"], float), np.array(d["PDCSAP_FLUX"], float), np.array(d["SAP_QUALITY"])
    ok = np.isfinite(t) & np.isfinite(f) & (q == 0)
    t, f = t[ok], f[ok] / np.median(f[ok])
    half = p.WindowDays() / 2
    trend = np.array([np.median(f[(t > x - half) & (t < x + half)]) for x in t])
    fd = f / trend
    periods = np.linspace(p.PeriodMin(), p.PeriodMax(), 20000)
    bls = BoxLeastSquares(t, fd)
    power = bls.power(periods, [0.08, 0.12, 0.16, 0.2])
    i = int(np.argmax(power.power))
    found = {"period": float(power.period[i]), "t0": float(power.transit_time[i]),
             "duration": float(power.duration[i]), "depth": float(power.depth[i])}
    print(f"{len(t)} points; BLS best period {found['period']:.4f} d, depth {found['depth'] * 1e6:.0f} ppm")
    np.savez(os.path.join(ctx.output_directory, "detrended.npz"), t=t, flux=fd, **found)
    ctx.display_line_chart([{"id": "flux", "label": f"detrended flux (BLS P = {found['period']:.4f} d)",
                             "x": t.tolist(), "y": fd.tolist()}])
    emit(ctx, "detrended", out)
