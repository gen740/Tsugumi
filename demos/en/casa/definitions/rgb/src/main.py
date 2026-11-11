"""Assigns the three band images to R, G and B with an asinh stretch per
channel and shows the PNG. Not exposure corrected: a picture, not a flux map."""
import hashlib
import json
import shutil
from pathlib import Path

import numpy as np
from astropy.io import fits
from PIL import Image

from inputs.input.casa.BandDir import BandDir
import outputs.rgb.casa.RgbDir as out


def main(ctx):
    source = Path(ctx.input("input", BandDir).Value().decode())
    p = ctx.params()
    percentile, q = p.Percentile(), p.Q()
    assert 90 <= percentile <= 100 and q > 0
    dest = Path(ctx.output_directory) / "rgb"
    dest.mkdir(parents=True, exist_ok=True)
    manifest = json.loads((source / "provenance.json").read_text())
    channels, scales, shape = [], [], None
    for color in ["red", "green", "blue"]:
        counts = np.asarray(fits.getdata(source / f"{color}.fits"), dtype=float)
        assert shape is None or shape == counts.shape, "the bands must share one grid"
        shape = counts.shape
        positive = counts[counts > 0]
        scale = max(float(np.percentile(positive, percentile)) if positive.size else 1.0, 1.0)
        channels.append(np.arcsinh(q * np.clip(counts / scale, 0, 1)) / np.arcsinh(q))
        scales.append(scale)
        shutil.copyfile(source / f"{color}.fits", dest / f"{color}.fits")
    # FITS rows grow with y: flip them so that north is up in the PNG.
    rgb = np.flipud(np.stack(channels, axis=-1))
    png = dest / "casa-rgb.png"
    Image.fromarray(np.round(np.clip(rgb, 0, 1) * 255).astype(np.uint8), "RGB").save(png)
    digest = hashlib.sha256(png.read_bytes()).hexdigest()
    manifest["display"] = {"stretch": "asinh", "q": q, "percentile": percentile, "scales": scales, "png_sha256": digest}
    (dest / "provenance.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2))
    print(f"casa-rgb.png sha256 {digest}")
    ctx.display_image(png.read_bytes())
    b = ctx.builder()
    s = b.CreateString(str(dest))
    out.Start(b)
    out.AddValue(b, s)
    ctx.output("rgb", b, out.End(b))
