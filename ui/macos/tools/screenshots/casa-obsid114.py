"""Points a copy of the Cas A demo at the deep observation (ObsID 114, 50 ks):
   python3 ui/macos/tools/screenshots/casa-obsid114.py <copy of the pinned en/casa project>"""
import json
import sys
from pathlib import Path

params = Path(sys.argv[1]) / "definitions/load/params.nix"
values = {"obsid": "114", "file": "acisf00114N006_evt2.fits.gz",
          "hash": "sha256-l7CCu8BibZmfu8QyVF5yC7OQjkrwbnkW5AvWlWcyK/Y="}
params.write_text("{\n" + "".join(f"  {name} = {json.dumps(value)};\n"
                                  for name, value in sorted(values.items())) + "}\n")
