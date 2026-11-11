"""Fetches the public data files into the output folder and hands the folder on."""
import hashlib
import os
import ssl
import urllib.request

import certifi

import outputs.data.exo.LightCurveFiles as out


def main(ctx):
    p = ctx.params()
    urls = [p.Urls(i).decode() for i in range(p.UrlsLength())]
    names = [p.Names(i).decode() for i in range(p.NamesLength())]
    context = ssl.create_default_context(cafile=certifi.where())
    rows = []
    for url, name in zip(urls, names):
        path = os.path.join(ctx.output_directory, name)
        req = urllib.request.Request(url, headers={"User-Agent": "Tsugumi demo"})
        with urllib.request.urlopen(req, timeout=180, context=context) as r, open(path, "wb") as f:
            f.write(r.read())
        data = open(path, "rb").read()
        digest = hashlib.sha256(data).hexdigest()
        print(f"{name}: {len(data)} bytes sha256 {digest} from {url}")
        rows.append(f"| `{name}` | {len(data):,} | `{digest[:16]}…` |")
    ctx.display_markdown(
        "**Fetch the light curve**\n\n| File | bytes | SHA-256 |\n| --- | ---: | --- |\n" + "\n".join(rows)
        + "\n\nSource: MAST Kepler archive, KIC 10874614 (Kepler-6), Q3 long cadence"
    )
    b = ctx.builder()
    s = b.CreateString(ctx.output_directory)
    out.Start(b)
    out.AddValue(b, s)
    ctx.output("data", b, out.End(b))
