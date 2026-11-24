"""Fetches the public data files into the output folder and hands the folder on."""
from card import Artifact, Input, Output, params as card_params
import hashlib
import os
import ssl
import urllib.request

import certifi


def run(inputs: Input) -> Output:
    artifact, directory = Artifact.dir()
    p = card_params
    urls = [p.urls[i].decode() for i in range(len(p.urls))]
    names = [p.names[i].decode() for i in range(len(p.names))]
    context = ssl.create_default_context(cafile=certifi.where())
    rows = []
    for url, name in zip(urls, names):
        path = os.path.join(directory, name)
        req = urllib.request.Request(url, headers={"User-Agent": "Tsugumi demo"})
        with urllib.request.urlopen(req, timeout=180, context=context) as r, open(path, "wb") as f:
            f.write(r.read())
        data = open(path, "rb").read()
        digest = hashlib.sha256(data).hexdigest()
        print(f"{name}: {len(data)} bytes sha256 {digest} from {url}")
        rows.append(f"| `{name}` | {len(data):,} | `{digest[:16]}…` |")
    # TODO(open): Port the demo display.
    return Output(data=artifact)
