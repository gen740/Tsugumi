import tsugumi
import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from astropy.io import fits
from astropy.timeseries import BoxLeastSquares

from inputs.data.exo.LightCurveFiles import LightCurveFiles
import outputs.detrended.exo.DetrendedDir as out


def emit(inputs, port, out):
    result_ports = {}
    s = '.'
    _out_value = {}
    _out_value['value'] = s
    result_ports.update({port: _out_value})
    return result_ports


def png(fig):
    import io
    buf = io.BytesIO()
    fig.savefig(buf, format="png", dpi=110, bbox_inches="tight")
    plt.close(fig)
    return buf.getvalue()


def main(inputs, report, run):
    result_ports = {}
    p = inputs.params
    folder = inputs.input("data", LightCurveFiles).Value().decode()
    with fits.open(os.path.join(folder, sorted(os.listdir(folder))[0])) as h:
        d = h[1].data
        t, f, q = np.array(d["TIME"], float), np.array(d["PDCSAP_FLUX"], float), np.array(d["SAP_QUALITY"])
    ok = np.isfinite(t) & np.isfinite(f) & (q == 0)
    t, f = t[ok], f[ok] / np.median(f[ok])
    half = p.windowDays / 2
    trend = np.array([np.median(f[(t > x - half) & (t < x + half)]) for x in t])
    fd = f / trend
    periods = np.linspace(p.periodMin, p.periodMax, 20000)
    bls = BoxLeastSquares(t, fd)
    power = bls.power(periods, [0.08, 0.12, 0.16, 0.2])
    i = int(np.argmax(power.power))
    found = {"period": float(power.period[i]), "t0": float(power.transit_time[i]),
             "duration": float(power.duration[i]), "depth": float(power.depth[i])}
    report.log(str(f"{len(t)} points; BLS best period {found['period']:.4f} d, depth {found['depth'] * 1e6:.0f} ppm"))
    np.savez(os.path.join(run.output_directory, "detrended.npz"), t=t, flux=fd, **found)
    report.line_chart([{"id": "flux", "label": f"detrended flux (BLS P = {found['period']:.4f} d)",
                             "x": t.tolist(), "y": fd.tolist()}])
    result_ports.update(emit(inputs, 'detrended', out))
    return result_ports
