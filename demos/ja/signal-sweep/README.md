# 信号の検出しきい値を比較

`start` を実行すると、信号強度 8・12・16 を検出しきい値 10・14 と比較します。

- `definitions/samples/default.nix` は順序付きの `run` リストを返し、`exports.thresholds` でしきい値を共有します。
- `definitions/score/default.nix` は受け側のポート名が `signal` なので、`inputs.signal.thresholds` を読みます。Bash の各実装は、実行時の信号を `$JSON_SIGNAL` から一つ読みます。
- 信号としきい値の全組み合わせを 6 回実行します。完了順にかかわらず、各信号の中でしきい値 10、14 の順に並びます。
- `collect` は `in` でバッチを受け取り、`items` ベクトルを持つ一つの型付き値を `out` から送ります。`definitions/summary/default.nix` は値を `$JSON_RESULTS`、共有のしきい値の個数を `inputs.results.thresholdCount` から読みます。

集計結果は 3 件の検出です。`score` の履歴を開くと、6 件それぞれの入力と出力を確認できます。
