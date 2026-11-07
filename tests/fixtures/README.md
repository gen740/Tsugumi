# UI / Agent の共通動作契約

macOS（Swift）と Web（TypeScript）は同じ JSON の入力と期待値で検証する。片方の実装だけに合わせて期待値を変更せず、意味が変わる場合は両実装と fixture を同時に更新する。

- `agent-contract.json` の `modelOptions`: 接続・モデル・メニューで選んだ effort と Fast mode の組ごとに、選択肢（レベル、既定、Fast の料金の倍率）と、リクエスト本文に入る項目・入らない項目。
- `merge-contract.json`: 名前付きレコードの同時編集、配列の順序・重複・削除・追加、connections の集合としての扱い。
- `agent-behavior.json` の `recovery`: OperationGet の reply / value と期待するエラー状態。`error: null` は未確定（sent を維持）、`false` は成功、`true` は失敗。両実装で実際の BFBS から応答を encode / decode し、Agent の復帰処理と保存後の履歴を検証する。
- `agent-behavior.json` の `compaction`: 保存形式の messages と圧縮境界 cut。ツール呼び出しと結果を分断せず、圧縮対象と残すメッセージを合わせると元の履歴になることを検証する。
- `agent-behavior.json` の `helperBrief`: ProjectSnapshot の値と読めるファイルから、helper が最初に受け取る文章。自分の Card、線、上流で値を作る Card（fork は通り抜ける）、読まれる先。
- `arrange-demos.json`: ある時点の同梱デモの全スコープから取った Card のグラフの見本（ポート名は実際の tsugumid が解決したもの）。デモを変えても作り直す必要はない。見本を新しくしたいときは `python3 tests/python/arrange_cases.py --write` で作る。
- `arrange-cases.json`: 孤立した Card、折りたたみ、フィードバック、Card in Card の境界、線の順序など、デモにない形を手で書いたもの。
- `arrange-contract.json`: 上の 2 つを両方の流れの向きで `CardGeometry` に通した入力と、整列の結果の期待値。両実装で入力（幾何）と結果の一致を確かめる。`CardGeometry` か整列を意図して変えたら `cd web && npm test -- --update-arrange` で作り直す。
- `wire-routes.json`: 配線の入力（流れの向き・障害物・線の両端）と、両実装が同じように見つける経路。入力はある時点のデモから取った見本で、デモを変えても作り直す必要はない。経路の探索を意図して変えたら `WIRE_ROUTES_WRITE=1 npm test` で作り直す。

実行:

```sh
npm --prefix web test
# Swift は macOS の Xcode + flatc が必要
(cd macOS/CoreTests && swift test --filter 'RecoveryTests|CompactionTests')
(cd macOS && swift test --filter mergePreservesNamedRecordsAndOrderedValues)
(cd macOS && swift test --filter ArrangeTests)
```

`.github/workflows/ci.yml` の Linux / macOS ジョブで両側を実行する。Agent の承認前後の永続化・保存失敗、既存モデル fixture を用いた圧縮後の継続などは、それぞれの言語の統合テストでも補う。
