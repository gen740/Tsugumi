"""Points a copy of the Cas A demo at the deep observation (ObsID 114, 50 ks):
   python3 macOS/tools/screenshots/casa-obsid114.py <copy of demos/casa>"""
import json
import sys
from pathlib import Path

card = Path(sys.argv[1]) / "definitions/load/_card.json"
d = json.loads(card.read_text())
values = {"obsid": "114", "file": "acisf00114N006_evt2.fits.gz",
          "hash": "sha256-l7CCu8BibZmfu8QyVF5yC7OQjkrwbnkW5AvWlWcyK/Y="}
for p in d["params"]:
    p["data"] = values[p["name"]]
card.write_text(json.dumps(d, ensure_ascii=False, indent=2) + "\n")
