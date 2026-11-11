#!/usr/bin/env python3
"""Makes a copy of the Plots demo carry many Plotly and Vega-Lite figures, for
measuring the canvas (canvas.ui):

    cp -R demos/en/plots /tmp/perf-plots && python3 macOS/tools/perf/many-plots.py /tmp/perf-plots 12

adds 12 copies each of `explore` (Plotly) and `spread` (Altair), every one with
its display open on the canvas.
"""
import json
import shutil
import sys
from pathlib import Path

project, count = Path(sys.argv[1]), int(sys.argv[2])
document = json.loads((project / "tsugumi.json").read_text())
layout = json.loads((project / "layout.json").read_text())
share = json.loads((project / "definitions/share/_card.json").read_text())
branches = next(p for p in share["params"] if p["name"] == "branches")["data"]

for i in range(count):
    for column, source in enumerate(("explore", "spread")):
        card = f"{source}{i + 2}"
        shutil.copytree(project / "definitions" / source, project / "definitions" / card)
        branches.append(card)
        document["canvas"]["cards"].append({"id": card})
        document["canvas"]["connections"].append(
            {"from": {"card": "share", "port": card}, "to": {"card": card, "port": "walks"}})
        x = -1110.0 + 600.0 * (2 * (i % 4) + column)
        y = 1400.0 + 520.0 * (i // 4)
        layout["cards"][card] = {"x": x + 210.0, "y": y}
        layout["displays"][card] = {"x": x, "y": y + 140.0, "width": 560.0, "height": 280.0}

(project / "tsugumi.json").write_text(json.dumps(document, indent=2) + "\n")
(project / "layout.json").write_text(json.dumps(layout, indent=2) + "\n")
(project / "definitions/share/_card.json").write_text(json.dumps(share, indent=2) + "\n")
