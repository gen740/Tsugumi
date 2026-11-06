# README 用スクリーンショット

`macOS/tools/run-ui.sh` で、デモのコピーを開いて撮影します（他の Tsugumi が起動していないこと）。
`TSUGUMI_UI_FRONT=1` は、ウインドウのボタンや選択をアクティブな色で撮るためのものです。撮影の間はアプリが最前面に出ます。
`workflow.ui` は Keychain の Anthropic キーで実際に Claude に依頼します（API 料金がかかります）。

```bash
cp -R demos/collatz /tmp/shot-collatz && TSUGUMI_UI_FRONT=1 macOS/tools/run-ui.sh dist/Tsugumi.app macOS/tools/screenshots/workflow.ui /tmp/shots /tmp/shot-collatz
```

```bash
cp -R demos/live-stream /tmp/shot-live && TSUGUMI_UI_FRONT=1 macOS/tools/run-ui.sh dist/Tsugumi.app macOS/tools/screenshots/live-stream.ui /tmp/shots /tmp/shot-live
```

README の画像をまとめて（`readme.ui`: Cas A の workflow・timeline・card-source と、`$demo` のツグミの guide。チャットは設定の接続先のモデルに 1 回依頼します）。README の画像は深い観測（ObsID 114、520 MB。最初の実行で 1 度だけ取得）で撮るので、コピーの `load` を書き換えてから撮ります:

```bash
cp -R demos/casa /tmp/shot-readme && python3 macOS/tools/screenshots/casa-obsid114.py /tmp/shot-readme && TSUGUMI_UI_FRONT=1 macOS/tools/run-ui.sh dist/Tsugumi.app macOS/tools/screenshots/readme.ui /tmp/shots /tmp/shot-readme
```

子 Workflow の画像（`subworkflow.ui`、collatz）:

```bash
cp -R demos/collatz /tmp/shot-sub && TSUGUMI_UI_FRONT=1 macOS/tools/run-ui.sh dist/Tsugumi.app macOS/tools/screenshots/subworkflow.ui /tmp/shots /tmp/shot-sub
```

Home のデモ一覧（`demos.ui`、プロジェクトは開きません）:

```bash
TSUGUMI_UI_FRONT=1 macOS/tools/run-ui.sh dist/Tsugumi.app macOS/tools/screenshots/demos.ui /tmp/shots
```
