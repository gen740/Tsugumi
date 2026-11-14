"""A synthetic plate: four-parameter logistic curves plus Gaussian noise."""
import tsugumi
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


def strings(items):
    offsets = [s for s in items]
    _b_items = []
    for o in reversed(offsets):
        _b_items.append(o)
    return list(reversed(_b_items))


def main(inputs, report):
    p = inputs.params
    seed, noise, n_conc, reps = p.seed, p.noiseSd, p.concentrations, p.replicates
    if n_conc * reps <= 4:
        raise Failure("output_invalid", "concentrations x replicates must exceed the 4 fitted parameters")
    conc = p.topUm / p.dilution ** np.arange(n_conc - 1, -1, -1.0)
    rng = np.random.default_rng(seed)
    response = np.array([hill4(conc, *c[1:])[:, None] + rng.normal(0, noise, (n_conc, reps)) for c in COMPOUNDS])
    report.log(str(f"seed {seed}, noise {noise}, {n_conc} concentrations {conc[0]:.4g}..{conc[-1]:.4g} uM, {reps} replicates"))
    report.log(str("plate " + json.dumps({"conc": conc.tolist(), "response": response.round(6).tolist()})))

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
    report.figure()
    names = strings([c[0] for c in COMPOUNDS])
    columns = [np.array([c[k] for c in COMPOUNDS]) for k in (1, 2, 3, 4)]
    conc_v = conc
    resp_v = response.ravel()
    _out_value = {}
    _out_value['names'] = names
    _out_value['bottom'] = columns[0]
    _out_value['top'] = columns[1]
    _out_value['ec50'] = columns[2]
    _out_value['hill'] = columns[3]
    _out_value['conc'] = conc_v
    _out_value['replicates'] = reps
    _out_value['response'] = resp_v
    _out_value['seed'] = seed
    _out_value['noise_sd'] = noise
    _out_value['dilution'] = p.dilution
    return {'plate': _out_value}
