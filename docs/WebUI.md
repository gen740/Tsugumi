# Web UI

`web/` は、macOS アプリと同じ画面構成・操作・ショートカットをブラウザで提供するクライアントです。
本文中のファイルパスとコマンドは、リポジトリのルートを基準としています。

## 構成

macOS アプリが共通 Linux VM の中で `tsugumid` を動かすのに対し、Web UI では **`tsugumid` をこのホストで直接**動かします（VM は使いません）。
ブラウザは `tsugumid` の WebSocket `/rpc` に FlatBuffers RPC で**直接**接続し、macOS アプリ・Agent と同じ契約を使う対等なクライアントになります。

```text
ブラウザ（React UI + WebGPU 描画） ── WebSocket /rpc (FlatBuffers) ── tsugumid（ホストで直接実行、プロジェクトごと）
        └─ HTTP /api ── Node サーバー（起動管理・ファイル・Agent）── Unix socket ──┘
```

- `tsugumid` は UI のオリジン（`http://127.0.0.1:<port>` と `http://localhost:<port>`）だけを `--allow-origin` で許可し、token は SessionHello でだけ送ります（URL・ログには出しません）。
- Node サーバー（`web/server/`）は、ブラウザにできないことだけを受け持ちます: プロジェクトフォルダの操作（新規作成・デモのコピー・フォルダ選択・Card in Card として取り込むプロジェクトの読み取り）、`tsugumid` の起動・停止、import grant、Skill の読み込み、Agent とモデルの API キー、ChatGPT のサインイン（データフォルダの `chatgpt.json`、0600）。`127.0.0.1` にだけ公開し、`Host` ヘッダーを検査し、変更を伴う API には同一オリジンのページしか送れないヘッダーを要求します。
- Agent はサーバー側で動き、tool 定義と JSON⇄FlatBuffers の変換は macOS 版と同じく `tsugumi.bfbs` から作ります。`tsugumid` へは Unix socket で接続します。
- 折れ線グラフは、macOS 版の Dawn 描画層（`ui/renderer`）と同じ WGSL・頂点生成を WebGPU に移植しています（Chromium の WebGPU 実装は Dawn）。WebGPU が使えないブラウザでは理由を表示します。

## ビルドと起動

前提: Nix（flakes 有効）、Node.js 22 以降、WebGPU が使えるブラウザ（Chrome / Edge / Safari 26 以降）。

```bash
nix develop --command bash -c 'cmake -S . -B build -G Ninja && ninja -C build tsugumid tsugumi-worker'
```

```bash
cd web && npm install && nix develop --command npm run gen && npm run build
```

```bash
node web/dist/server.js --open
```

起動時に表示される `http://127.0.0.1:8780/?key=…` を開きます（`--open` なら自動で開きます）。`key` は起動のたびに作る鍵で、このページだけが送る cookie（HttpOnly・SameSite=Strict）に置き換わってアドレスバーから消えます。`/api/` はすべて（GET とイベントの購読も）この cookie を求めるので、同じマシンの他のユーザーや他のサイトのページからは使えません。変更を伴う呼び出しはさらに専用のヘッダーを求めます。応答には CSP（`frame-ancestors 'none'`）と `nosniff` を付けます。`--port` でポートを、引数のプロジェクトフォルダでそのまま開くプロジェクトを指定できます。起動中はデータフォルダに `server.json`（ポートと Agent 用の鍵、0600）を置き、[Claude Code の plugin](claude-code.md) はこれで動いているサーバーを見つけます。この鍵はページの鍵とは別で、plugin が使う口（`AGENT_ROUTES`）にしか届かず、Card の権限を広げる保存の承認には答えられません（[セキュリティ §8](security.md#agent-の権限境界)）。core のバイナリがない場合は、初期設定画面からビルドすることもできます。

| 環境変数 | 内容 |
| --- | --- |
| `TSUGUMID` / `TSUGUMI_WORKER` / `TSUGUMI_SDK_DIR` | バイナリと SDK の場所（既定は `build/` と `sdk/`） |
| `TSUGUMI_WEB_DATA` | 設定・最近のプロジェクト・会話・control フォルダの保存先（macOS の既定 `~/Library/Application Support/Tsugumi/web`） |
| `ANTHROPIC_API_KEY` | あればそれを API キーとして使う（なければ macOS では Keychain のアプリと同じ項目、それ以外ではデータフォルダの 0600 ファイル） |
| `TSUGUMI_CHATGPT_ISSUER` / `TSUGUMI_CHATGPT_API` | ChatGPT のサインインとプランへのリクエストの送り先（テスト用。既定は `https://auth.openai.com` と `https://api.openai.com`） |

TypeScript の binding と `tsugumi.bfbs` は `npm run gen`（`flatc` 25.12.19）で `web/src/gen` に生成し、Git には入れていません。

## 隔離（macOS ホスト）

ホストで直接動く `tsugumid` は、`sandbox: true` の Card を bubblewrap の代わりに `sandbox-exec`（Seatbelt。非推奨ですが現行の macOS で動作します）で隔離します（`core/src/worker_proc.cc`）。

- ネットワークは使えません（Card がネットワークを許可した場合を除く）。
- 読み取れるのは `/nix/store`、実行・入力の領域、システムライブラリ、worker 本体だけです。ホームフォルダやプロジェクトの文書は読めません。
- 書き込めるのは worker 自身のフォルダ（スクリプトを除く）と、実行の出力フォルダ（`runs/<run>/work/…/out`）だけです。Linux では出力フォルダを `/proc/self/fd` 経由で渡しますが、macOS にはないためパスで渡します。
- bubblewrap と違い、ファイルの存在確認（`stat`）は全体で許可しています（中身は読めません）。また、uid の分離はしません。
- Mac のフォルダを Card に共有する機能（`/host/<name>`）は共通 VM が必要なため、macOS 上の Web UI では使えません。Card 設定ではフォルダを追加できず、すでに設定のある Card にはその旨を表示します（実行すると失敗します）。bubblewrap のある Linux では、共通 VM なしで使えるはずです（手元では確かめていません）。
- ネットワークを許可した Card も、このマシン自身には接続できません（tsugumid や Web サーバーに触れないため）。loopback だけでなく、LAN や VPN のアドレス、`0.0.0.0` 宛ても同じです（Seatbelt の `localhost` はこのマシンのアドレス全体を指します）。Unix ソケットも閉じています。例外は `mDNSResponder` のソケット（`/var/run/mDNSResponder`）だけです。macOS の名前解決はこのソケットを通るので、閉じると名前を引けません。このソケットは DNS-SD の窓口でもあるため、同じ経路で LAN のサービスを探したり公開したりはできます。
- ネットワークを許可した Card が使える Mach サービスは、証明書の検証に使う `com.apple.trustd.agent` だけです。クリップボードや LaunchServices（アプリや URL を開く）、Apple Event には届きません。名前解決と HTTPS はこれで動きます（Python・curl・Go・NSURLSession で確認）。代わりに、ネットワークなしの Card と同じくユーザー情報（`getpwuid`）を引けないので、`ssh` は `No user exists for uid` で失敗します。システム設定のプロキシも読みません。
- 塞げない経路が 3 つあります。これらが問題になるネットワークでは、他人の Card に `network: true` を与えないでください。
  - **同じ LAN や VPN の他の機器には届きます。** Seatbelt で宛先に書けるのは `*` と `localhost` だけで、アドレスや範囲を指定して拒否できません。
  - **UDP のブロードキャストとマルチキャストは、このマシンにも届きます。** `0.0.0.0` で UDP を待ち受けるサービスには届き、返事も受け取れます。ブロードキャストだけを止める指定がなく、止めるなら UDP 全体を閉じることになるので、開けてあります。tsugumid と Web サーバーは UDP を使わないので対象外です。
  - **Card は待ち受けを開けます。** このマシンから接続できることを確認しました。LAN の他の機器からも、ファイアウォールが通せば届きます。
- チャットに添付したフォルダは、サーバーが control フォルダの `attached.json` に書いたものだけを tsugumid が読みます（RPC からは変えられません）。
- `sandbox: false` の Card は、このマシンで利用者の権限のまま動きます。Card 設定で「ファイルアクセスを制限」をオフにするときは、そのことを確認してから切り替えます（`tsugumid` 自身は判断しません）。Linux で他人の Card を動かすときは、`tsugumid` を隔離されたコンテナの中で動かしてください。

## 複数のプロジェクト

ブラウザのタブごとに別のプロジェクトを開けます。動作は [UI の仕様](ui-view.md#複数のプロジェクト) にあります。サーバーはプロジェクトごとに `tsugumid` を 1 つ動かし、どのタブがどのプロジェクトを表示しているかを知っているので（`POST /api/page`）、Claude Code が開いたプロジェクトは、それを表示中のタブか Home のタブに表示し、別のプロジェクトを表示中のタブは切り替えません。

## ショートカット

既定値は macOS 版と同じで、アプリ設定 > ショートカットで変更できます（ブラウザの localStorage に保存）。
Canvas にフォーカスがあるときだけ働く 1 キーの操作（`s` 設定、`a` 成果物、`l` ログ、`f` 折りたたみ、`c` 複製、`0` 全体表示、`1` 100%、`g` グループ、`e` Draft 追加など）と、どこからでも使える操作（⌘R 実行、⌘. 停止、⌥⌘J チャット、⌥⌘L ログ、⌥⌘T タイムライン、⌃` ターミナルなど）があります。
macOS 以外では ⌘ の代わりに Ctrl を使います。ブラウザが予約しているキー（⌘W・⌘T・⌘N・⌘Q など）は割り当ててもページに届きません。
