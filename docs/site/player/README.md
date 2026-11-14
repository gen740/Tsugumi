# The demo player

A player for the documentation site that shows demos in motion without installing anything.
Cards, wires and geometry use the Web UI's code as it is (`web/src/ui/cardNode.tsx`, `web/src/model/canvasKit.ts`), and the CSS uses the colours of `web/src/styles/canvas.css` and `web/src/styles.css`, scoped inside `.tp-root`.
Nothing is computed in the browser: it only replays the record of a real run.

```js
import { mountPlayer } from './site/js/player.js'
const player = mountPlayer(el, { demo: 'report', data: 'site/player-data/report.json' })
player.destroy()
```

- `data` is a relative URL of the JSON, and images are loaded relative to that JSON. If the page has no `site/player.css`, it is added automatically.
- `compact: true` is the reduced view for the gallery. It draws the whole within the frame and plays when it comes into view and on hover (`autoplay: false` for hover only). Give `el` a height.
- The normal view plays once when it comes into view with `autoplay: true`. By default it does not move until ▶ Run is pressed.
- With `prefers-reduced-motion`, it does not play automatically (`compact` shows the state at the end of the run, still).
- The height can be changed with `--tp-height`.

## Regenerating

After changing Cards or demos, first prepare `web/src/gen` (`nix develop --command npm run gen` in `web/`), then run:

```sh
node docs/site/player/build.mjs
```

`node docs/site/player/build.mjs --check` (which writes nothing and fails if `player.js`, `player.css` or `player-data/` differ from the current inputs) shows when regenerating was forgotten. CI's Web step runs it.

Recording runs is a separate step. It needs a `tsugumid` build and Nix (the first time takes a few minutes to fetch dependencies).

```sh
# The default is <repo>/build. For a build elsewhere, set TSUGUMI_BUILD_DIR
# Runs every demo except the Drafts, isolated (Seatbelt)
nix develop --command python3 docs/site/player/harvest.py --ports \
  report collatz live-stream plots casa card-guide \
  gw150914 protein-6lu7 exoplanet-transit seismic-trench \
  mnist-classifiers newton pharmacokinetics max-flow dose-response molecules cell-count \
  survival meta-analysis rna-seq simpson battery gillespie heat-inverse ecology-models
node docs/site/player/build.mjs
```

### After an SDK interface change

Rebuild `tsugumid`, `tsugumi-worker` and the SDK from the same checkout before recording. Install the Web dependencies (`npm ci` in `web/`) and generate its protocol bindings (`nix develop --command npm run gen` there). Then, from the repository root, refresh every runnable demo and the guide, including their resolved ports:

```sh
nix develop --command python3 - <<'PYTHON'
import json
import subprocess
import sys
from pathlib import Path
entries = json.loads(Path("demos/en/index.json").read_text())["demos"]
ids = [entry["id"] for entry in entries if "Draft" not in entry.get("tags", [])]
subprocess.run([sys.executable, "docs/site/player/harvest.py", "--ports", *ids, "card-guide"], check=True)
PYTHON
node docs/site/player/build.mjs
node docs/site/player/svg.mjs
node docs/site/player/build.mjs --check
```

`harvest.py` produces `player-data/assets/ports.json`, each `<id>.run.json` and its images. `build.mjs` embeds the current Card sources, descriptions and narration in `player-data/<id>.json` and `index.json`, and rebuilds `site/js/player.js` and `site/player.css`. `svg.mjs` then refreshes the gallery SVGs and the README's scenes, including the source-code scene. Recording and SVG rendering need daemon sockets and headless Chrome; they cannot run in a sandbox that forbids listeners or app launches. The old records and SVGs remain evidence of their original runs until a person runs these commands. Finally regenerate and check the documentation with `nix run .#build-doc` and `nix run .#test-doc`.

## Moving SVGs

For places like GitHub's README, where scripts do not run and only images can be placed, the replay is made into SVGs. After regenerating `player-data/`, run this on a Mac and commit the SVGs it makes (it uses Chrome without a screen, and shrinks the tiles' images with `sips`).

```sh
node docs/site/player/svg.mjs            # every demo with a record; list IDs for only those
```
