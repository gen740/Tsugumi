"""A synthetic plate: four-parameter logistic curves plus Gaussian noise."""
import json

import matplotlib.pyplot as plt
import numpy as np

from tsugumi import Failure

# Fictional compounds: name, bottom, top, EC50 (uM), Hill slope.
COMPOUNDS = [
    ("A", 2.0, 98.0, 0.30, 1.0),
    ("B", 4.0, 85.0, 4.50, 1.6),
]
COLORS = ["#2a78d6", "#eb6834"]


def hill4(x, bottom, top, ec50, hill):
    return bottom + (top - bottom) / (1 + (ec50 / x) ** hill)


def main(inputs, report):
    p = inputs.params
    seed, noise, n_conc, reps = p.seed, p.noiseSd, p.concentrations, p.replicates
    if n_conc * reps <= 4:
        raise Failure("output_invalid", "concentrations x replicates must exceed the 4 fitted parameters")
    conc = p.topUm / p.dilution ** np.arange(n_conc - 1, -1, -1.0)
    rng = np.random.default_rng(seed)
    response = np.array([hill4(conc, *c[1:])[:, None] + rng.normal(0, noise, (n_conc, reps)) for c in COMPOUNDS])
    report.log(f"seed {seed}, noise {noise}, {n_conc} concentrations {conc[0]:.4g}..{conc[-1]:.4g} uM, {reps} replicates")
    report.log("plate " + json.dumps({"conc": conc.tolist(), "response": response.round(6).tolist()}))

    fig, ax = plt.subplots(figsize=(7, 4.2))
    grid = np.geomspace(conc[0] / 2, conc[-1] * 2, 200)
    for i, c in enumerate(COMPOUNDS):
        ax.plot(grid, hill4(grid, *c[1:]), color=COLORS[i], lw=2, label=f"{c[0]} の真の曲線（EC50 = {c[3]:g} µM）")
        ax.plot(np.repeat(conc, reps), response[i].ravel(), "o", ms=4, color=COLORS[i], alpha=0.65, mec="none")
    ax.set_xscale("log")
    ax.set_xlabel("濃度 (µM)")
    ax.set_ylabel("反応 (%)")
    ax.set_title(f"合成データ（架空の化合物）: ノイズ SD {noise:g}、{n_conc} 濃度 × {reps} 反復")
    ax.grid(alpha=0.25)
    ax.spines[["top", "right"]].set_visible(False)
    ax.legend(frameon=False, loc="upper left")
    report.figure()
    names = list([c[0] for c in COMPOUNDS])
    columns = [np.array([c[k] for c in COMPOUNDS]) for k in (1, 2, 3, 4)]
    conc_v = conc
    resp_v = response.ravel()
    return {'plate': {
        'names': names,
        'bottom': columns[0],
        'top': columns[1],
        'ec50': columns[2],
        'hill': columns[3],
        'conc': conc_v,
        'replicates': reps,
        'response': resp_v,
        'seed': seed,
        'noise_sd': noise,
        'dilution': p.dilution,
    }}
