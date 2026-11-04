# 版の管理（git）

プロジェクトのフォルダは git のリポジトリです。アプリで作ったプロジェクトとデモのコピーは、最初に開いたときにリポジトリになります。それ以外のプロジェクトは「版の履歴」から「版管理を始める」でリポジトリにできます。`.tsugumi/`（tsugumid の状態）は `.gitignore` で除外します。

## 版ができるとき

- **実行の開始時**：前の版から変わっていれば、保存済みの内容を commit します（メッセージ「実行: <Card>」）。実行は開始時の commit を記録し、実行一覧や `RunStatus.commit` で見られます。何も変えずにもう一度実行したときは同じ commit に実行が増えます（commit と実行は 1 : N）。
- **版として残す**：今の内容を commit し、名前（annotated tag）とメモを付けます。

## 実行の結果

実行が終わるたびに、その実行の commit の note（`refs/notes/tsugumi`）に 1 行追記します。

```
run 3f2a9c1e-… succeeded 2026-11-04T10:12:00Z 12.3s
```

git からも結果で版を選べます。

```bash
git log --notes=tsugumi --grep=failed
```

```bash
git tag -l
```

出力やログは git に入れず、これまでどおり実行の記録（VM の中の履歴、`.tsugumi/runs.sqlite`）に置きます。

## 版のツリー

Mac アプリは Workflow メニューの「版の履歴…」（⌥⌘Y）、Web UI はサイドバー下のボタンで開きます。

- 枝分かれと合流を線で描き、各版に実行の回数と結果（点の色: 緑は成功、赤は失敗）、手動で残した版の名前（tag）、枝の名前、変わった Card を表示します。
- 版を選ぶと、そのメッセージと実行の結果を見られます。「この版から始める」で、今の内容を版として残してから、その版の内容に戻ります。
- 枝の先の版に戻ると、その枝を続けます。それ以外の版から変更して実行すると、新しい枝（`try-<版>`）になり、前の流れは消えません。
- 未保存の変更があるときと実行中は、別の版に移れません。

## nixpkgs

新しいプロジェクトは、作った Tsugumi の nixpkgs（`flake.lock`）を `tsugumi.json` の `pkgsPath` に固定値で持ちます。版の一部なので、過去の版に戻れば nixpkgs も戻ります。固定されていないプロジェクト（`tsugumi:default`）は、Workflow 設定で固定できます。

## デモの履歴

デモのフォルダにある `.tsugumi-history.json` は、コピーがリポジトリになるときに commit・枝・tag・note になります（ファイル自体は消えます）。Cas A デモには、帯域や格子・伸長を試した過程（失敗した枝、採用しなかった枝、採用した版 `rgb-v1`）が入っています。

## 実装

`core/src/versions.cc`（libgit2）。RPC は `VersionInit`・`VersionMark`・`VersionCheckout`・`VersionGraph`。git の操作が失敗しても実行は止めません（その実行は版を記録しません）。
