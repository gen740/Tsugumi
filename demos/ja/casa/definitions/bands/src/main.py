"""Bins the Cas A events into three disjoint energy bands on one sky grid
centred on Cas A (its position converted with the event file's own x, y
coordinate keywords, so every observation is centred the same way). CIAO's
dmcopy does the binning."""
import tsugumi
import json
import math
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

import numpy as np
from astropy.io import fits

from inputs.ciao.casa.Ciao import Ciao
from inputs.input.casa.Events import Events

COLORS = ["red", "green", "blue"]


def sky_pixel(columns, ra, dec):
    """RA/Dec (deg) to sky pixels: gnomonic projection with TCRVL, TCRPX, TCDLT."""
    cx, cy = columns["x"], columns["y"]
    a0, d0 = math.radians(cx.coord_ref_value), math.radians(cy.coord_ref_value)
    a, d = math.radians(ra), math.radians(dec)
    cosc = math.sin(d0) * math.sin(d) + math.cos(d0) * math.cos(d) * math.cos(a - a0)
    xi = math.degrees(math.cos(d) * math.sin(a - a0) / cosc)
    eta = math.degrees((math.cos(d0) * math.sin(d) - math.sin(d0) * math.cos(d) * math.cos(a - a0)) / cosc)
    return cx.coord_ref_point + xi / cx.coord_inc, cy.coord_ref_point + eta / cy.coord_inc


def main(inputs, report, run):
    events = inputs.input("input", Events)
    source, obsid = events.Path().decode(), events.Obsid().decode()
    tool = inputs.input("ciao", Ciao)
    root = tool.Path().decode()
    p = inputs.params
    limits = [p.redMinEv, p.greenMinEv, p.blueMinEv, p.maxEv]
    assert all(a < b for a, b in zip(limits, limits[1:])), "energy boundaries must increase"
    step, size = p.binSize, p.pixels
    assert step > 0 and 16 <= size <= 2048
    dest = Path(run.output_directory) / "bands"
    dest.mkdir(parents=True, exist_ok=True)
    with fits.open(source, memmap=True) as hd:
        ev = hd["EVENTS"]
        cols = ev.columns
        assert cols["energy"].unit == "eV", "energy must be in eV"
        cx, cy = sky_pixel(cols, p.ra, p.dec)
        # The grid starts on a whole sky pixel: its edges are then exact in the bin= text,
        # and dmcopy makes exactly `size` bins wherever the observation's centre falls.
        x0, y0 = math.floor(cx - step * size / 2), math.floor(cy - step * size / 2)
        edges_x, edges_y = x0 + np.arange(size + 1) * step, y0 + np.arange(size + 1) * step
        x = np.asarray(ev.data["x"], dtype=float)
        y = np.asarray(ev.data["y"], dtype=float)
        energy = np.asarray(ev.data["energy"], dtype=float)
        inside = (x >= edges_x[0]) & (x < edges_x[-1]) & (y >= edges_y[0]) & (y < edges_y[-1])
        selected = int((inside & (energy >= limits[0]) & (energy < limits[-1])).sum())
        bins = f"bin x={edges_x[0]:.6f}:{edges_x[-1]:.6f}:{step},y={edges_y[0]:.6f}:{edges_y[-1]:.6f}:{step}"
        with tempfile.TemporaryDirectory() as work:
            env = {**os.environ, "ASCDS_INSTALL": root, "PFILES": f"{work};{root}/param"}
            bands = []
            for i, color in enumerate(COLORS):
                # Written beside the parameter files first: the output directory's path is an open file
                # descriptor of this process, which dmcopy does not inherit.
                image = Path(work) / f"{color}.fits"
                # A range of energy= does not include its upper limit.
                spec = f"{source}[energy={limits[i]}:{limits[i + 1]}][{bins}]"
                subprocess.run([f"{root}/bin/dmcopy", spec, str(image)], env=env, check=True)
                counts = fits.getdata(image)
                assert counts.shape == (size, size), f"dmcopy gave a {counts.shape} grid for {size} x {size}"
                shutil.copyfile(image, dest / image.name)
                bands.append({"color": color, "min_ev": limits[i], "max_ev": limits[i + 1], "photons": int(counts.sum()), "dmcopy": spec})
        # Counted again without CIAO: what the tool wrote is every selected photon, once.
        assert sum(b["photons"] for b in bands) == selected, "bands overlap or leave a gap"
        manifest = {"obsid": obsid, "events_file": source, "exposure_s": ev.header.get("EXPOSURE"), "events": len(ev.data),
                    "centre_pixel": [x0 + step * size / 2, y0 + step * size / 2], "grid": {"step": step, "pixels": size},
                    "selected": selected, "bands": bands, "tool": {"name": "CIAO", "version": tool.Version().decode(), "path": root}}
    (dest / "provenance.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2))
    report.log(str(json.dumps(manifest, ensure_ascii=False)))
    report.table(["帯域", "エネルギー (eV)", "光子数"],
                      [[b["color"], f"{b['min_ev']:g}–{b['max_ev']:g}", b["photons"]] for b in bands])
    s = str(dest)
    _out_value = {}
    _out_value['value'] = s
    return {'images': _out_value}
