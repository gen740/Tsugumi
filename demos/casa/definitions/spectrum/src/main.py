"""The energy spectrum of the photons in the RGB image.

Same events and sky grid as bands; its three bands are shaded in
their colours and the emission lines of the ejecta are marked.
Plain matplotlib: plt.show() puts the figure in the Card's display."""
import json
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
from astropy.io import fits

from inputs.input.casa.BandDir import BandDir

# Helium-like lines (Fe: the K-shell blend), in eV.
LINES = [
    ("Mg", 1352), ("Si", 1865), ("S", 2461),
    ("Ar", 3140), ("Ca", 3902), ("Fe", 6680),
]
SHADE = {"red": "#e5484d", "green": "#30a46c", "blue": "#3e7bfa"}


def main(ctx):
    source = Path(ctx.input("input", BandDir).Value().decode())
    manifest = json.loads((source / "provenance.json").read_text())
    p = ctx.params()
    lo, hi, width = p.MinEv(), p.MaxEv(), p.BinEv()
    assert 0 <= lo < hi and width > 0, "bad energy range or bin"

    # The photons in the same square of sky as the bands' grid.
    (cx, cy), grid = manifest["centre_pixel"], manifest["grid"]
    half = grid["step"] * grid["pixels"] / 2
    with fits.open(manifest["events_file"], memmap=True) as hd:
        ev = hd["EVENTS"].data
        x = np.asarray(ev["x"], dtype=float)
        y = np.asarray(ev["y"], dtype=float)
        energy = np.asarray(ev["energy"], dtype=float)
    inside = (np.abs(x - cx) < half) & (np.abs(y - cy) < half)
    edges = np.arange(lo, hi + width, width)
    counts, _ = np.histogram(energy[inside], bins=edges)

    # Larger type: the figure is shrunk to fit the display.
    with plt.rc_context({"font.size": 13}):
        fig, ax = plt.subplots(figsize=(7, 3.4))
        for band in manifest["bands"]:
            ax.axvspan(band["min_ev"] / 1000, band["max_ev"] / 1000,
                       color=SHADE[band["color"]], alpha=0.16,
                       label=f"{band['color']}  {band['photons']:,}")
        ax.stairs(counts, edges / 1000, color="0.15", lw=1)
        ax.set_yscale("log")
        ax.set_xlim(lo / 1000, hi / 1000)
        top = counts.max()
        ax.set_ylim(max(counts.min(), 1), top * 4)
        for name, e in LINES:
            if lo < e < hi:
                ax.axvline(e / 1000, color="0.45", lw=0.8, ls=":")
                ax.annotate(name, (e / 1000, top * 1.3), ha="center",
                            fontweight="bold")
        ks = (manifest.get("exposure_s") or 0) / 1000
        title = f"Cas A · ObsID {manifest['obsid']} · {ks:.0f} ks"
        ax.set(xlabel="energy (keV)", ylabel=f"photons / {width:g} eV",
               title=title)
        ax.legend(loc="lower left", title="photons", frameon=False,
                  fontsize=11, title_fontsize=11)
        fig.tight_layout()
        plt.show()
    print(json.dumps({"photons": int(counts.sum()), "bin_ev": width}))
