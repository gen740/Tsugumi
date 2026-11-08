# 用語集

Tsugumi の文書と画面で使う言葉です。詳しくは各行のリンク先を参照してください。

## 処理の組み立て

| 言葉 | 意味 |
| --- | --- |
| **Workflow** | Card を線でつないだグラフ。人間が描き、Agent が編集し、そのまま実行される同じもの（[設計の独自性](novelty.md)） |
| **Card** | 処理の単位。入出力のポートと設定を持つ（[Card と Workflow](features.md#2-card-と-workflow)） |
| **コード Card** | `default.nix` で環境を、`src/` で処理を定義する Card（C++ / Python / bash / R / Julia、[はじめての Card](cardguide.md)） |
| **組み込み Card** | コードのない Card。開始（trigger）・入力値（input）・分配（fork）・分岐（branch）・合流（merge）・記録（logger） |
| **Draft** | 名前と内容だけを持つ未実装の Card。線でつないで計画を描き（ポートは線に合わせて作られる）、Agent に実装させる |
| **Card in Card（子 Workflow）** | 中に Workflow を持つ Card。外からは 1 つの Card に見え、開くと中へ降りられる |
| **定義** | Card の実体である `definitions/<id>/` のフォルダ。Card ごとに 1 つ |
| **ポート** | Card の入出力の口。出力の型は `_outputs.fbs` に書き、入力の型はつないだ出力から導出する |
| **`all` / `any`** | 入力の待ち方。`all` はすべての必須入力が揃ったら 1 値ずつ、`any` は到着順に消費する（[実行の意味論](features.md#実行の意味論スケジューラー)） |
| **`maxRuns`** | Card を 1 回の Run で呼べる上限。ループの反復を有限にする |

## 実行と記録

| 言葉 | 意味 |
| --- | --- |
| **Run（実行）** | Workflow を動かす 1 回。開始時の版（commit）を記録する |
| **呼び出し（invocation）** | Run の中で Card が 1 回動くこと。ループでは同じ Card が何度も呼ばれる |
| **段階（stage）** | 呼び出しの中の工程。SDK 準備・Nix 評価・build・実行・出力検証・保存（[実行段階](features.md#実行段階)） |
| **`skipped`** | 選ばれなかった分岐の下流。失敗ではない |
| **`blocked`** | Draft や失敗した上流に依存して動けない Card。理由が付く |
| **`interrupted`** | 異常終了や再起動で途中だった実行 |
| **部分実行（単体実行）** | 選んだ Card や「ここから」だけを動かすこと。保存された入力を使う（[Q&A の Q5](qa.md#q5-解析の途中からやり直せることにはどんな意味がありますか)） |
| **保存された入力** | 部分実行に使う、最新の呼び出しの入力一式。`recordIO` とは別に保存する |
| **`recordIO`** | Card の全呼び出しの入出力値を履歴に残す設定（既定は off） |
| **`retainArtifact`** | Card の成果物を保持する設定 |
| **成果物** | Card が出力したファイルやフォルダ |
| **表示（Display）** | Card が画面に出す結果。テキスト・表・画像・グラフなど（[可視化](features.md#7-可視化)） |
| **版** | プロジェクトの git の commit。名前を付けた版は annotated tag（[版の管理](versions.md)） |

## 実行の基盤

| 言葉 | 意味 |
| --- | --- |
| **`tsugumid`（core）** | プロジェクトごとに 1 つ動くプロセス。文書・スケジューラー・Nix 評価・履歴を担う |
| **RPC** | UI・Web・Agent が `tsugumid` と話す契約（[Protocol.md](protocol.md)） |
| **`op_id`** | 変更を伴う要求の ID。同じ ID の再送は 1 回だけ適用される |
| **rev** | 文書のリビジョン。保存の基準にし、古い基準の保存は競合として拒否する |
| **共通 VM** | Mac アプリと Mac の `tsugumi` コマンドが共有する 1 台の Linux VM。同時に使えるのはどちらか一方（`vm.lock`、[セキュリティと隔離](security.md)） |
| **`tsugumi-vmd`** | VM の中の管理プロセス。プロジェクトごとに uid・名前空間・cgroup を分けて `tsugumid` を起動する |
| **worker** | Card のコードを動かすプロセス。実行をまたいで常駐できる（[常駐 worker](features.md#常駐-worker)） |
| **bubblewrap / Seatbelt** | Card の隔離の仕組み。VM の中と Linux では bubblewrap、リポジトリから Mac で直接動かす Web UI では Seatbelt |
| **pkgsPath** | Card が使う nixpkgs の固定先（`tsugumi.json`） |

## Agent

| 言葉 | 意味 |
| --- | --- |
| **Agent** | モデルに接続し、RPC 由来の tool で Card を作り・実行するチャット（[主要機能の Agent](features.md#9-agent)） |
| **helper** | 依存しない作業を手分けするときに Agent が使う子の Agent |
| **ツグミ** | チャットとガイドの鳥。画面の中を飛んで説明する |
| **承認（`approval_required`）** | Agent の保存が Card の権限を広げるときに、利用者の確認を求める仕組み（[Agent の権限境界](security.md#agent-の権限境界)） |
| **Skill** | `$` で呼び出す指示のまとまり |
