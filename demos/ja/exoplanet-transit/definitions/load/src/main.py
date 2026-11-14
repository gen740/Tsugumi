"""Fetches the public data files into the output folder and hands the folder on."""
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
        report.log(f"{name}: {len(data)} bytes sha256 {digest} from {url}")
        rows.append(f"| `{name}` | {len(data):,} | `{digest[:16]}…` |")
    report.markdown(
        "**ライトカーブを取る**\n\n| ファイル | bytes | SHA-256 |\n| --- | ---: | --- |\n" + "\n".join(rows)
        + "\n\n出典: MAST Kepler archive, KIC 10874614 (Kepler-6), Q3 long cadence"
    )
    s = run.output_directory
    return {'data': {'value': s}}
