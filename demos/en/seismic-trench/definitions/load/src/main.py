"""Fetches the public data files into the output folder and hands the folder on."""
import tsugumi
import hashlib
import os
import ssl
import urllib.request

import certifi



def main(inputs, report, run):
    p = inputs.params
    urls = [p.urls[i].decode() for i in range(len(p.urls))]
    names = [p.names[i].decode() for i in range(len(p.names))]
    context = ssl.create_default_context(cafile=certifi.where())
    rows = []
    for url, name in zip(urls, names):
        path = os.path.join(run.output_directory, name)
        req = urllib.request.Request(url, headers={"User-Agent": "Tsugumi demo"})
        with urllib.request.urlopen(req, timeout=180, context=context) as r, open(path, "wb") as f:
            f.write(r.read())
        data = open(path, "rb").read()
        digest = hashlib.sha256(data).hexdigest()
        report.log(str(f"{name}: {len(data)} bytes sha256 {digest} from {url}"))
        rows.append(f"| `{name}` | {len(data):,} | `{digest[:16]}…` |")
    report.markdown(
        "**Fetch the earthquake catalogue**\n\n| File | bytes | SHA-256 |\n| --- | ---: | --- |\n" + "\n".join(rows)
        + "\n\nSource: USGS ComCat (earthquake.usgs.gov FDSN event service), 2000-2024, M >= 4"
    )
    s = '.'
    _out_value = {}
    _out_value['value'] = s
    return {'data': _out_value}
