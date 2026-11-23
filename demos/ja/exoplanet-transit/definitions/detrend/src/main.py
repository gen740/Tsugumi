from card import Artifact, Input, Output, params as card_params
import os

import numpy as np
from astropy.io import fits
from astropy.timeseries import BoxLeastSquares


def run(inputs: Input) -> Output:
    artifact, directory = Artifact.dir()
    p = card_params
    folder = inputs.require("data").open()
    with fits.open(os.path.join(folder, sorted(os.listdir(folder))[0])) as h:
        d = h[1].data
        t, f, q = np.array(d["TIME"], float), np.array(d["PDCSAP_FLUX"], float), np.array(d["SAP_QUALITY"])
    ok = np.isfinite(t) & np.isfinite(f) & (q == 0)
    t, f = t[ok], f[ok] / np.median(f[ok])
    half = p.window_days / 2
    trend = np.array([np.median(f[(t > x - half) & (t < x + half)]) for x in t])
    fd = f / trend
    periods = np.linspace(p.period_min, p.period_max, 20000)
    bls = BoxLeastSquares(t, fd)
    power = bls.power(periods, [0.08, 0.12, 0.16, 0.2])
    i = int(np.argmax(power.power))
    found = {"period": float(power.period[i]), "t0": float(power.transit_time[i]),
             "duration": float(power.duration[i]), "depth": float(power.depth[i])}
    print(f"{len(t)} points; BLS best period {found['period']:.4f} d, depth {found['depth'] * 1e6:.0f} ppm")
    np.savez(os.path.join(directory, "detrended.npz"), t=t, flux=fd, **found)
    # TODO(open): Port the demo display.
    return Output(detrended=artifact)
