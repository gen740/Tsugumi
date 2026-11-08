# Claude Code から使う

Tsugumi のプロジェクトを Claude Code から開き、Card の作成・編集・実行・結果の確認を任せられます（Claude Code の plugin `tsugumi`、`plugins/tsugumi/`）。
作業の様子は [Web UI](WebUI.md) にそのまま出ます。Claude Code が保存した Card は Canvas で変わり、実行は進み具合ごと表示され、ツグミが Claude Code の作業している Card のそばへ飛んでいきます。利用者は同じ画面を見ながら、自分でも触れます。
Windows（WSL2）で動作を検証しました。

## 仕組み

```text
Claude Code ── MCP (stdio) ── web/dist/mcp.js ── HTTP /api/agent/* ── Web UI のサーバー ── tsugumid
                                                                       └─ イベント ── ブラウザ（Canvas・ツグミ・承認）
```

- plugin の MCP サーバー（`web/server/mcp.ts`）は `tsugumid` に直接つながりません。Web UI のサーバーに頼み、サーバーがチャットの Agent と同じ道具（`project_inspect`・`project_save`・`run`・`run_record`・`log_range` など）を、同じ検査（引数の検査、`op_id`、リビジョンの競合、実行の待ち合わせ）で実行します。保存の `origin` は `agent` です。
- サーバーは呼び出しをページにも知らせます（`agentWork`）。ページはチャットの Agent のときと同じく、ツグミをその Card へ飛ばし、「generate の記録を見ています」のような一言を添えます。10 秒ほど呼び出しがなければ、ツグミはチャットの左上に戻ります。モデルのキーを Tsugumi に登録していなくても、Claude Code が作業する間はツグミが出ます。
- 実行のアニメーション（Card の状態、配線を流れる様子）は `tsugumid` から届くので、誰が実行しても同じように出ます。
- Card に新しくネットワーク・隔離なしの実行・ホストのフォルダを与える保存は、チャットの Agent と同じく利用者の承認を待ちます。承認の画面は Web UI のチャットに「Claude Code が Card の権限を広げようとしています」として出ます。Web UI のページが開いていなければ承認できないので、保存はされず、Claude Code には利用者に説明するよう伝えます。
- チャットに添えたフォルダの読み取り（`attached_folder_read`）、ヘルパーへの分担（`delegate`）、ツグミの吹き出し（`guide_show`）はチャットのものなので、Claude Code には渡しません。Claude Code は自分のファイル操作や subagent を使います。

## 準備

1. Tsugumi のリポジトリを手元に置き、core と Web UI を [Web UI のビルドと起動](WebUI.md#ビルドと起動)の手順でビルドします。

2. Claude Code で、このリポジトリを marketplace として加え、plugin を入れます。

   ```text
   /plugin marketplace add gen740/Tsugumi
   /plugin install tsugumi@tsugumi
   ```

   入れるときに「Tsugumi checkout」として、1 でビルドしたリポジトリのフォルダを指定します（あとから `/plugin configure tsugumi@tsugumi` で変えられます）。plugin は Claude Code のキャッシュにコピーされ、ビルドしたものはその中にないため、このフォルダから MCP サーバーを起動します。`node` が見つからなければ、そのフォルダの Nix の開発環境（`nix develop`）の `node` を使います。

## 使い方

- `/tsugumi:open <プロジェクトのフォルダ>` か、「この Tsugumi のプロジェクトで…して」と頼みます。Claude Code は `tsugumi_open` でプロジェクトを開いてから作業します。
- `tsugumi_open` は、動いている Web UI のサーバー（データフォルダの `server.json`）を探し、なければ起動します（Claude Code を閉じても動き続けます。止めるときはそのプロセスを止めます）。そのプロジェクトを表示中のページか Home のページがあればそこに表示し（別のプロジェクトを表示中のページは切り替えません）、なければサーバーがブラウザで開きます（`server.json` の鍵はページの鍵ではないためです。[セキュリティ §8](security.md#agent-の権限境界)）。
- 新しいプロジェクトは `tsugumi_open` の `create: true` で作れます（まだないフォルダに限ります）。
- 最初の `tsugumi_open` は、道具を使うときの決まり（チャットの Agent と同じ `agent/Shared/system-prompt.md`）を返します。

| 環境変数（MCP サーバー） | 内容 |
| --- | --- |
| `TSUGUMI_WEB_DATA` | Web UI のデータフォルダ（`server.json` を探す場所）。サーバーと同じ値にします |
| `TSUGUMI_WEB_PORT` | 起動するサーバーのポート（既定は空いているポート） |

## 対応していないこと

- macOS アプリには対応していません。Claude Code が作業するプロジェクトは Web UI で開きます。
- Card は、`tsugumi_open` が見つけた Web UI のサーバーが動かす環境で実行されます（Mac で `tsugumi` コマンドのサーバーが動いていれば共通 VM の中）。動いているサーバーがなければ、ビルドしたリポジトリからサーバーを起動し、`tsugumid` はこのホストで直接動きます。
