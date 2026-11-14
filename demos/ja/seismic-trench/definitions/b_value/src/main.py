import tsugumi
import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from inputs.section.quake.SectionDir import SectionDir
import outputs.fmd.quake.FmdDir as out


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
    sel = np.loadtxt(os.path.join(inputs.input("section", SectionDir).Value().decode(), "selected.csv"),
                     delimiter=",", skiprows=1, ndmin=2)
    mags = sel[:, 3]
    mc, dm = p.mc, p.binWidth
    use = mags[mags >= mc - 1e-9]
    # Aki (1965) maximum likelihood with the bin correction of Utsu (1966).
    b = np.log10(np.e) / (use.mean() - (mc - dm / 2))
    b_err = b / np.sqrt(len(use))
    a = np.log10(len(use)) + b * mc
    grid = np.round(np.arange(mags.min(), mags.max() + dm, dm), 2)
    cum = np.array([np.sum(mags >= m - 1e-9) for m in grid])
    ok = cum > 0
    fit = grid >= mc
    with open(os.path.join(run.output_directory, "fmd.csv"), "w") as fh:
        fh.write("magnitude,N_ge_M\n" + "".join(f"{m:.1f},{n}\n" for m, n in zip(grid, cum)))
    report.log(str(f"{len(use)} events with M >= {mc:g}: b = {b:.2f} +/- {b_err:.2f}, a = {a:.2f}"))
    report.line_chart([
        {"id": "observed", "label": "log10 N(>=M) observed", "x": grid[ok].tolist(), "y": np.log10(cum[ok]).tolist()},
        {"id": "fit", "label": f"Gutenberg-Richter fit b = {b:.2f} +/- {b_err:.2f} (M >= {mc:g})",
         "x": grid[fit].tolist(), "y": (a - b * grid[fit]).tolist()},
    ])
    result_ports.update(emit(inputs, 'fmd', out))
    return result_ports
