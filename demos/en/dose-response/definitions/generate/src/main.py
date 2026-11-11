"""A synthetic plate: four-parameter logistic curves plus Gaussian noise."""
import json

import matplotlib.pyplot as plt
import numpy as np

from tsugumi import Failure
import outputs.plate.dose.Plate as out

# Fictional compounds: name, bottom, top, EC50 (uM), Hill slope.
COMPOUNDS = [
    ("A", 2.0, 98.0, 0.30, 1.0),
    ("B", 4.0, 85.0, 4.50, 1.6),
]
COLORS = ["#2a78d6", "#eb6834"]


def hill4(x, bottom, top, ec50, hill):
    return bottom + (top - bottom) / (1 + (ec50 / x) ** hill)


def strings(b, items):
    offsets = [b.CreateString(s) for s in items]
    b.StartVector(4, len(offsets), 4)
    for o in reversed(offsets):
        b.PrependUOffsetTRelative(o)
    return b.EndVector()


def main(ctx):
    p = ctx.params()
    seed, noise, n_conc, reps = p.Seed(), p.NoiseSd(), p.Concentrations(), p.Replicates()
    if n_conc * reps <= 4:
        raise Failure("output_invalid", "concentrations x replicates must exceed the 4 fitted parameters")
    conc = p.TopUm() / p.Dilution() ** np.arange(n_conc - 1, -1, -1.0)
    rng = np.random.default_rng(seed)
    response = np.array([hill4(conc, *c[1:])[:, None] + rng.normal(0, noise, (n_conc, reps)) for c in COMPOUNDS])
    print(f"seed {seed}, noise {noise}, {n_conc} concentrations {conc[0]:.4g}..{conc[-1]:.4g} uM, {reps} replicates")
    print("plate " + json.dumps({"conc": conc.tolist(), "response": response.round(6).tolist()}))

    fig, ax = plt.subplots(figsize=(7, 4.2))
    grid = np.geomspace(conc[0] / 2, conc[-1] * 2, 200)
    for i, c in enumerate(COMPOUNDS):
        ax.plot(grid, hill4(grid, *c[1:]), color=COLORS[i], lw=2, label=f"true curve of {c[0]} (EC50 = {c[3]:g} µM)")
        ax.plot(np.repeat(conc, reps), response[i].ravel(), "o", ms=4, color=COLORS[i], alpha=0.65, mec="none")
    ax.set_xscale("log")
    ax.set_xlabel("Concentration (µM)")
    ax.set_ylabel("Response (%)")
    ax.set_title(f"Synthetic data (made-up compounds): noise SD {noise:g}, {n_conc} concentrations × {reps} replicates")
    ax.grid(alpha=0.25)
    ax.spines[["top", "right"]].set_visible(False)
    ax.legend(frameon=False, loc="upper left")
    plt.show()

    b = ctx.builder(response.size * 8 + 2048)
    names = strings(b, [c[0] for c in COMPOUNDS])
    columns = [b.CreateNumpyVector(np.array([c[k] for c in COMPOUNDS])) for k in (1, 2, 3, 4)]
    conc_v = b.CreateNumpyVector(conc)
    resp_v = b.CreateNumpyVector(response.ravel())
    out.Start(b)
    out.AddNames(b, names)
    out.AddBottom(b, columns[0])
    out.AddTop(b, columns[1])
    out.AddEc50(b, columns[2])
    out.AddHill(b, columns[3])
    out.AddConc(b, conc_v)
    out.AddReplicates(b, reps)
    out.AddResponse(b, resp_v)
    out.AddSeed(b, seed)
    out.AddNoiseSd(b, noise)
    out.AddDilution(b, p.Dilution())
    ctx.output("plate", b, out.End(b))
