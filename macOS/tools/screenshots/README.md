# README 用スクリーンショット

`macOS/tools/run-ui.sh` で、デモのコピーを開いて撮影します（他の Tsugumi が起動していないこと）。
`workflow.ui` は Keychain の Anthropic キーで実際に Claude に依頼します（API 料金がかかります）。

```bash
cp -R demos/collatz /tmp/shot-collatz && macOS/tools/run-ui.sh dist/Tsugumi.app macOS/tools/screenshots/workflow.ui /tmp/shots /tmp/shot-collatz
```

```bash
cp -R demos/live-stream /tmp/shot-live && macOS/tools/run-ui.sh dist/Tsugumi.app macOS/tools/screenshots/live-stream.ui /tmp/shots /tmp/shot-live
```

Home のデモ一覧（`demos.ui`、プロジェクトは開きません）:

```bash
macOS/tools/run-ui.sh dist/Tsugumi.app macOS/tools/screenshots/demos.ui /tmp/shots
```
