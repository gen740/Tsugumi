# Capture scripts

Open a copy of a demo and capture it with `macOS/tools/run-ui.sh` (no other Tsugumi may be running).

The Dawn line chart of Live Stream (`live-stream.ui`, for checking on the real screen):

```bash
cp -R demos/live-stream /tmp/shot-live && macOS/tools/run-ui.sh dist/Tsugumi.app macOS/tools/screenshots/live-stream.ui /tmp/shots /tmp/shot-live
```

The README's images all at once (`readme.ui`: Cas A's workflow, timeline and card-source, and the thrush's guide with `$demo`; the chat makes one request to the model connected in the settings). The README's images are taken with the deep observation (ObsID 114, 520 MB, fetched once on the first run), so rewrite the copy's `load` before capturing:

```bash
cp -R demos/casa /tmp/shot-readme && python3 macOS/tools/screenshots/casa-obsid114.py /tmp/shot-readme && macOS/tools/run-ui.sh dist/Tsugumi.app macOS/tools/screenshots/readme.ui /tmp/shots /tmp/shot-readme
```

The child Workflow's image (`subworkflow.ui`, collatz):

```bash
cp -R demos/collatz /tmp/shot-sub && macOS/tools/run-ui.sh dist/Tsugumi.app macOS/tools/screenshots/subworkflow.ui /tmp/shots /tmp/shot-sub
```

The demo list on Home (`demos.ui`; no project is opened):

```bash
macOS/tools/run-ui.sh dist/Tsugumi.app macOS/tools/screenshots/demos.ui /tmp/shots
```
