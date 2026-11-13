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

## Files

| File | Contents |
| --- | --- |
| `web/src/ui/demoPlayer.tsx` | The player itself. The thumbnails on the Web UI's Home are the same thing |
| `web/src/styles/demoPlayer.css` | The player's own CSS |
| `narration.json` | The text for each stage of a run (written by hand from [Demos.md](../../Demos.md)) |
| `build.mjs` | Generates `site/js/player.js`, `site/player.css`, `site/player-data/<id>.json` and `index.json` (`--check` only checks) |
| `harvest.py` | Runs the demos for real and writes their records into `site/player-data/assets/` |
| `svg.mjs` | Turns the replay into moving SVGs (the large Cas A `docs/assets/casa-demo.svg`, the app-window-shaped `docs/assets/casa-window.svg`, each demo's tile `docs/assets/demos/<id>.svg`), for places like the README that can hold only images |
| `demo.html` | A page for development (thumbnails and normal views of every demo) |

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

## Moving SVGs

For places like GitHub's README, where scripts do not run and only images can be placed, the replay is made into SVGs. After regenerating `player-data/`, run this on a Mac and commit the SVGs it makes (it uses Chrome without a screen, and shrinks the tiles' images with `sips`).

```sh
node docs/site/player/svg.mjs            # every demo with a record; list IDs for only those
```

- `docs/assets/casa-demo.svg`: Cas A, large, in the normal view (the Run button and elapsed time, the description, the notes).
- `docs/assets/casa-window.svg`: Cas A in the shape of the Mac app's window. Where Cards, output frames and notes go follows the README's screenshot (`docs/assets/screenshots/workflow.png`; `WINDOW_LAYOUT` in `svg.mjs`); the Run button and elapsed time are in the title bar, and the run's state is in the status bar.
- The screens of the README's "Following an experiment on screen" move in the same window shape.
  - `casa-source.svg`: the spectrum Card's settings open, and `main.py` scrolls down to `plt.show()`.
  - `casa-timeline.svg`: the timeline at the bottom grows with the replay, by each Card's start and end and the time of its stages.
  - `collatz-subworkflow.svg`: replays the Workflow inside collatz-cpp, with its counts (the loop 112 times and so on).
  - `casa-guide.svg`: the thrush flies from the chat to the spectrum Card and shows a speech bubble.
- `docs/assets/demos/<id>.svg`: each demo with a record, as a 440 × 264 tile in the gallery's reduced view (`compact`). Text the CSS hides on a small Canvas (`.tp-small` and the like) and the contents of the Cards' description lines are left out, and images are twice the size they are shown at in the tile.

Each stage of the replay (`framesOf`) is turned into the HTML that the player's `Canvas` (and `RunControl`) draws with `site/player.css`. The drawing is held only once (with Cards split into shape, body and ports); that HTML is laid out in headless Chrome and turned by [dom-to-svg](https://github.com/felixfbecker/dom-to-svg) into an SVG of rectangles, lines, text and images, shown with CSS animations only during the stages in which it appears (the elements' classes remain on the SVG's groups). It loops, and with `prefers-reduced-motion` it stops at the state at the end of the run. Images are embedded as data: URIs, so it loads no outside files or fonts.

How it differs from the player: Cards cannot be selected to read inside, there are no zoom buttons, and output frames fade in only on the first round. To read inside, the site's `#demo-<id>` (for example `https://gen740.github.io/Tsugumi/#demo-casa`) opens that demo's player.

`<foreignObject>`, which puts HTML inside SVG, is not used. Safari draws positioned elements inside the `<foreignObject>` of an SVG shrunk to the size of an `<img>` without shrinking them (WebKit [bug 23113](https://bugs.webkit.org/show_bug.cgi?id=23113)), and redraws only part when they move. What dom-to-svg does not draw (the `radial-gradient` of the Canvas's dot grid, list markers) is replaced with SVG patterns or text before converting.

On GitHub they are shown as `<img>` (in the README). Opening a file on raw.githubusercontent.com directly, its CSP (`default-src 'none'`) blocks even data: URI images, so only the output images do not appear (a figure with alt text becomes a broken image icon). In the README, instead of the link to the raw file GitHub puts on images, an `<a>` links to the site's `#demo-<id>`.

## What the records hold

`run` in `player-data/assets/<id>.run.json` says which run the record is of (Run ID, date, host, isolation, commit).
The order and timing of the replay are `RunInvocationsRequest`, and the displays are the values of `RunRecordRequest` themselves. Cards whose times overlap are replayed in one stage, as having run at the same time. The replay's speed is not proportional to the real time.

Only these things are changed:

- Images are shrunk to 640 px on their long side, and PNGs over 220 KB become JPEGs.
- Tables are cut to their first 24 rows, text to 4000 characters, and line and scatter charts thinned to 400 points per series (only those with many points).
- Plotly, Altair and WebGPU line charts are drawn as simple SVGs of the same values, without loading the libraries (no zooming or hovering as in the app). Vega-Lite charts with separate panels (concat or facet) are drawn with Vega into images when recorded (`vega-svg.mjs`). 3D point clouds show only the number of points.
- `ports.json` is the ports the core resolved (nothing is run).

Demos without a record are replayed in an order made from their wires, with no results. Drafts cannot run.
