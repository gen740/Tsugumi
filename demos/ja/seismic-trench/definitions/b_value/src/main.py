from card import Artifact, Input, Output, params as card_params
import os

import numpy as np


def run(inputs: Input) -> Output:
    artifact, directory = Artifact.dir()
    p = card_params
    sel = np.loadtxt(os.path.join(inputs.require("section").open(), "selected.csv"),
                     delimiter=",", skiprows=1, ndmin=2)
    mags = sel[:, 3]
    mc, dm = p.mc, p.bin_width
    use = mags[mags >= mc - 1e-9]
    # Aki (1965) maximum likelihood with the bin correction of Utsu (1966).
    b = np.log10(np.e) / (use.mean() - (mc - dm / 2))
    b_err = b / np.sqrt(len(use))
    a = np.log10(len(use)) + b * mc
    grid = np.round(np.arange(mags.min(), mags.max() + dm, dm), 2)
    cum = np.array([np.sum(mags >= m - 1e-9) for m in grid])
    ok = cum > 0
    fit = grid >= mc
    with open(os.path.join(directory, "fmd.csv"), "w") as fh:
        fh.write("magnitude,N_ge_M\n" + "".join(f"{m:.1f},{n}\n" for m, n in zip(grid, cum)))
    print(f"{len(use)} events with M >= {mc:g}: b = {b:.2f} +/- {b_err:.2f}, a = {a:.2f}")
    # TODO(open): Port the demo display.
    return Output(fmd=artifact)
