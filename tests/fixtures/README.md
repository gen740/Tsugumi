# UI / Agent の共通動作契約

macOS（Swift）と Web（TypeScript）は同じ JSON の入力と期待値で検証する。片方の実装だけに合わせて期待値を変更せず、意味が変わる場合は両実装と fixture を同時に更新する。

- `merge-contract.json`: 名前付きレコードの同時編集、配列の順序・重複・削除・追加、connections の集合としての扱い。
- `agent-behavior.json` の `recovery`: OperationGet の reply / value と期待するエラー状態。`error: null` は未確定（sent を維持）、`false` は成功、`true` は失敗。両実装で実際の BFBS から応答を encode / decode し、Agent の復帰処理と保存後の履歴を検証する。
- `agent-behavior.json` の `compaction`: 保存形式の messages と圧縮境界 cut。ツール呼び出しと結果を分断せず、圧縮対象と残すメッセージを合わせると元の履歴になることを検証する。

実行:

```sh
npm --prefix web test
# Swift は macOS の Xcode + flatc が必要
(cd macOS/CoreTests && swift test --filter 'RecoveryTests|CompactionTests')
(cd macOS && swift test --filter mergePreservesNamedRecordsAndOrderedValues)
```

`.github/workflows/ci.yml` の Linux / macOS ジョブで両側を実行する。Agent の承認前後の永続化・保存失敗、既存モデル fixture を用いた圧縮後の継続などは、それぞれの言語の統合テストでも補う。
