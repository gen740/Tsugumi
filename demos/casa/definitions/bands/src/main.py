"""Bins the Cas A events into three disjoint energy bands on one sky grid
centred on Cas A (its position converted with the event file's own x, y
coordinate keywords, so every observation is centred the same way)."""
import json
import math
from pathlib import Path

import numpy as np
from astropy.io import fits

from inputs.input.casa.Events import Events
import outputs.images.casa.BandDir as out

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


def main(ctx):
    events = ctx.input("input", Events)
    source, obsid = events.Path().decode(), events.Obsid().decode()
    p = ctx.params()
    limits = [p.RedMinEv(), p.GreenMinEv(), p.BlueMinEv(), p.MaxEv()]
    assert all(a < b for a, b in zip(limits, limits[1:])), "energy boundaries must increase"
    step, size = p.BinSize(), p.Pixels()
    assert step > 0 and 16 <= size <= 2048
    dest = Path(ctx.output_directory) / "bands"
    dest.mkdir(parents=True, exist_ok=True)
    with fits.open(source, memmap=True) as hd:
        ev = hd["EVENTS"]
        cols = ev.columns
        assert cols["energy"].unit == "eV", "energy must be in eV"
        cx, cy = sky_pixel(cols, p.Ra(), p.Dec())
        x0, y0 = cx - step * size / 2, cy - step * size / 2
        edges_x, edges_y = x0 + np.arange(size + 1) * step, y0 + np.arange(size + 1) * step
        x = np.asarray(ev.data["x"], dtype=float)
        y = np.asarray(ev.data["y"], dtype=float)
        energy = np.asarray(ev.data["energy"], dtype=float)
        inside = (x >= edges_x[0]) & (x < edges_x[-1]) & (y >= edges_y[0]) & (y < edges_y[-1])
        header = fits.Header()
        for axis, key, start in [(1, "x", x0), (2, "y", y0)]:
            c = cols[key]
            header[f"CTYPE{axis}"] = c.coord_type
            header[f"CRVAL{axis}"] = c.coord_ref_value
            header[f"CRPIX{axis}"] = (c.coord_ref_point - start) / step + 0.5
            header[f"CDELT{axis}"] = c.coord_inc * step
        header["BUNIT"] = "count"
        header["OBJECT"] = "CAS A"
        header["OBS_ID"] = obsid
        bands = []
        for i, color in enumerate(COLORS):
            upper = energy <= limits[i + 1] if i == 2 else energy < limits[i + 1]
            mask = inside & (energy >= limits[i]) & upper
            counts = np.histogram2d(y[mask], x[mask], bins=[edges_y, edges_x])[0].astype(np.int32)
            assert int(counts.sum()) == int(mask.sum()), "no selected photon may be lost"
            h = header.copy()
            h["E_MIN"], h["E_MAX"] = limits[i], limits[i + 1]
            fits.PrimaryHDU(counts, header=h).writeto(dest / f"{color}.fits", overwrite=True)
            bands.append({"color": color, "min_ev": limits[i], "max_ev": limits[i + 1], "photons": int(counts.sum())})
        selected = int((inside & (energy >= limits[0]) & (energy <= limits[-1])).sum())
        assert sum(b["photons"] for b in bands) == selected, "bands overlap or leave a gap"
        manifest = {"obsid": obsid, "events_file": source, "exposure_s": ev.header.get("EXPOSURE"), "events": len(ev.data),
                    "centre_pixel": [round(cx, 2), round(cy, 2)], "grid": {"step": step, "pixels": size},
                    "selected": selected, "bands": bands}
    (dest / "provenance.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2))
    print(json.dumps(manifest, ensure_ascii=False))
    ctx.display_table(["帯域", "エネルギー (eV)", "光子数"],
                      [[b["color"], f"{b['min_ev']:g}–{b['max_ev']:g}", b["photons"]] for b in bands])
    b = ctx.builder()
    s = b.CreateString(str(dest))
    out.Start(b)
    out.AddValue(b, s)
    ctx.output("images", b, out.End(b))
