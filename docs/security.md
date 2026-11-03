# セキュリティと隔離

この文書は、Tsugumi が「どこからどこまでを隔離しているか」と「秘密情報（モデルの API キー、RPC の認証 token）をどう扱うか」をまとめたものです。
記述は現在の実装（`core/src/vmd_main.cc`・`core/src/worker_proc.cc`・`core/src/server.cc`・`macOS/Sources/Launcher/`・`agent/Sources/TsugumiAgent/Provider.swift`）に基づきます。

## 1. 信頼境界の全体像

```text
┌─ macOS（ユーザー権限） ──────────────────────────────────────────────┐
│  Tsugumi.app                                                        │
│   ├ SwiftUI UI                                                      │
│   ├ Agent ── HTTPS ──▶ モデル provider（API キーは Keychain から）   │
│   └ LoopbackBridge 127.0.0.1:<空きポート>  ◀── 外部クライアント       │
│        │ vsock                                                      │
│  ┌─ 共通 Linux VM（Virtualization.framework） ────────────────────┐ │
│  │  tsugumi-vmd（root、VM の管理役）  nix-daemon（共通 store）       │ │
│  │  ┌─ project A ───────────────────────┐ ┌─ project B ────────┐  │ │
│  │  │ 専用 uid / mount・PID・IPC・UTS ns │ │ 同左（別 uid）     │  │ │
│  │  │ cgroup tsugumi/<A>                 │ │                    │  │ │
│  │  │ tsugumid                           │ │ tsugumid           │  │ │
│  │  │  └ worker（bubblewrap: 全 ns 分離、 │ │  └ worker          │  │ │
│  │  │     既定で通信なし、cap なし）       │ │                    │  │ │
│  │  └────────────────────────────────────┘ └────────────────────┘  │ │
│  └─────────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────┘
```

境界は内側から順に 4 段あります。

| 境界 | 守るもの | 仕組み |
| --- | --- | --- |
| Card（worker）⇄ tsugumid | プロジェクト全体・token・他の Card の作業領域・ネットワーク（既定） | bubblewrap |
| project ⇄ project | 他プロジェクトのファイル・プロセス・履歴・ビルド root | uid・名前空間・cgroup（tsugumi-vmd） |
| VM ⇄ macOS | Mac のファイル全体・ホームフォルダ | Virtualization.framework、共有は開いているプロジェクトのフォルダと、Card 定義やチャットで指定された Mac のフォルダ（§2）だけ |
| macOS 上の RPC ⇄ 他のプロセス・ネットワーク | core の操作権限 | 127.0.0.1 だけで待ち受け、token による認証、WebSocket の Origin 検査 |

## 2. VM ⇄ macOS

### VM に渡しているもの

| デバイス | 内容 | 書込 |
| --- | --- | --- |
| rootfs | アプリ同梱の erofs イメージ（カーネル 6.12.28、`macOS/Container/vm.nix`） | 読み取り専用 |
| store ディスク | `~/Library/Application Support/Tsugumi/vm/store.img`（共通 Nix store） | 可 |
| 実行履歴ディスク | `~/Library/Application Support/Tsugumi/vm/history.img`（全プロジェクトの実行履歴、プロジェクトごとのフォルダ） | 可 |
| virtiofs（スロットごと） | 開いているプロジェクトのフォルダ（`project`）と、その control フォルダ（`control`） | 可 |
| virtiofs（同じスロット、指定したときだけ） | Card 設定で指定した Mac のフォルダ（`host-<tag>`）、チャットに添付した Mac のフォルダ（`chat-<tag>`） | `host-` は Card が書き込みを許可したものだけ可、`chat-` は読み取り専用 |
| vsock | 制御（1024）と RPC（1025） | — |
| ネットワーク | NAT（`VZNATNetworkDeviceAttachment`） | — |

- **Mac のファイルで VM から見えるのは、いま開いているプロジェクトのフォルダとその control フォルダ、そしてそのプロジェクトの Card 定義やチャットで指定された Mac のフォルダだけ**です。ホームフォルダ全体や他のプロジェクトは共有しません。プロジェクトを閉じるとそのスロットの共有を外します。
- Mac のフォルダを VM に見せる経路は次の 2 つです（`macOS/Sources/Launcher/EnvironmentManager.swift`・`HostFolders.swift`）。
  - **Card の「ホストのフォルダへのアクセスを許可する」**（`_card.json` の `hostFolders`、Experimental）: プロジェクト（とその externals）の Card 定義に書かれたフォルダを、環境の起動時にそのプロジェクトのスロットへ共有します。書き込みを許可したもの（`write: true`）以外は読み取り専用です。**共有はファイルに書かれた内容だけで決まり、開くときに確認はありません**。他人から受け取ったプロジェクトや、Agent が変更した Card 定義に含まれていても同じように共有されます（§10）。
  - **チャットへのフォルダの添付**: Agent が読むためのフォルダを、読み取り専用で共有します。
- 共有はプロジェクトごとに別の virtiofs デバイス（`tsugumi-<slot>`）なので、あるプロジェクトの共有を差し替えても他のプロジェクトには影響しません。
- アプリの entitlement は `com.apple.security.virtualization` だけです（`macOS/Resources/Tsugumi.entitlements`）。
- VM は `vm/vm.lock` の flock を持つアプリ 1 つだけが起動します。

### VM のネットワーク

NAT は **nix-daemon が `cache.nixos.org` からバイナリを取得し、固定した nixpkgs のアーカイブを取得するため**にあります。
Card は既定ではネットワークを持ちません。Card 設定の「ネットワークへのアクセスを許可する」（`_card.json` の `network: true`、Experimental）をオンにした Card だけが、この NAT を通って外部と通信できます（§4）。

## 3. project ⇄ project（VM 内）

tsugumi-vmd がプロジェクトごとに tsugumid を次の条件で起動します（`core/src/vmd_main.cc`）。

- **専用 uid**: プロジェクトごとに固定の uid（20001 以降、`/persist/tsugumi/uids` に記録）。`setgroups(0)` → `setgid` → `setuid` で root を捨ててから起動します。
- **名前空間**: `CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWIPC | CLONE_NEWUTS`。
- **見えるファイル**: mount 名前空間で次だけを bind し、全スロットの mount（`/mnt`）、store ディスク（`/persist`）、全プロジェクトの実行履歴ディスク（`/history-disk`）は umount します。
  - `/project` … 自分のプロジェクトフォルダ
  - `/control` … 自分の control フォルダ
  - `/history` … 自分の実行履歴（実行履歴ディスク上、`nosuid,nodev`）
  - 私的な `/tmp` と `/run/tsugumi`（tmpfs）
  - `/run/tsugumi/host/<tag>`・`/run/tsugumi/chat/<tag>` … そのプロジェクトに共有した Mac のフォルダ（§2。指定したときだけ）
  - `/nix` は overlay のまま見える（共通 store は全員が読み取り、書き込みは nix-daemon 経由のみ）
- **cgroup**: `/sys/fs/cgroup/tsugumi/<id>`。プロジェクトの停止は `cgroup.kill` でその cgroup のプロセスだけを終わらせ、他のプロジェクトには触れません。
- **GC root**: `/nix/var/nix/gcroots/tsugumi/projects/<id>/` は vmd が root として作り、親フォルダはプロジェクトから書けません。他のプロジェクトが先にフォルダを作って横取りすることはできません（`test_shared_vm.py` で確認）。
- **ターミナル**: VM 内ターミナルは、そのプロジェクトの名前空間に `setns` で入り、cgroup に入り、プロジェクトの uid に落としてから bash を起動します。プロジェクトを止めればターミナルも終わります。

### 共有しているもの

- nix-daemon と `/nix/store`。store の内容はハッシュで識別され、ビルドは nix-daemon の sandbox（`sandbox = true`）で行われるため、あるプロジェクトが他のプロジェクトのビルド結果を書き換えることはできません。ただし、**どのパスが store にあるかは全プロジェクトから読めます**（Nix の性質）。store に秘密を入れないでください。

## 4. Card（worker）⇄ tsugumid

Card のコードは bubblewrap の中で動きます（`core/src/worker_proc.cc`）。

### `sandbox: true`（既定）

```text
bwrap --unshare-all --die-with-parent --new-session --cap-drop ALL
      --ro-bind /nix/store /nix/store --proc /proc --dev /dev --tmpfs /tmp
      --uid <nobody 相当> --gid <…>
      --ro-bind <保存済み入力・runs>  --bind <worker の一時領域>
      -- tsugumi-worker
```

- **全名前空間を分離**（user・mount・PID・network・IPC・UTS・cgroup）。ネットワークインターフェースはループバックだけです。`network: true` の Card だけは network 名前空間を分けず（`--share-net`）、名前解決と CA 証明書のファイルを読み取り専用で見せます。
- **capability はすべて落とす**。
- **見えるファイル**: `/nix/store`（読み取り専用）、プロジェクトの `runs` と保存済み入力（読み取り専用）、その worker 専用の一時領域（書き込み可）、その Card が `hostFolders` で指定した Mac のフォルダ（`/host/<name>`、`write: true` のものだけ書き込み可）だけ。プロジェクトの文書・ソース・control フォルダ・token・他の Card の作業領域、ほかの Card が指定した Mac のフォルダ、チャットに添付したフォルダは見えません。
- **環境変数は空から構成**（`PATH=/var/empty`、`LANG=C.UTF-8` と Tsugumi の変数だけ）。tsugumid やホストの環境変数は継承しません。
- **出力**: 呼び出しの出力フォルダは `SCM_RIGHTS` で FD を渡し、`/proc/self/fd/900` として見せます。core が本来のパスに戻して検証します。
- **入力を FD で渡さない理由**: 読み取り専用で開いた FD でも、`/proc/self/fd/N` から開き直すと書き込めてしまうことを実測で確認したため。入力は読み取り専用の bind mount で守ります。
- **bubblewrap がない環境では `sandbox: true` の Card を実行せず拒否します**（`permission_denied`）。黙って隔離なしで動かすことはありません。

### `sandbox: false`

ファイルアクセスの制限だけを外す設定です。それでも次は維持します。

- user・PID・**network**・IPC 名前空間は分離（`network: true` でなければネットワークなし、子孫プロセスは worker と一緒に終わる）。
- control フォルダと `.tsugumi` は空の tmpfs で隠します。virtiofs は全ファイルを「見る側の所有」として見せるため、ファイルの mode では token を守れないからです。
- VM 内のファイルはプロジェクトの uid で見えるため、そのプロジェクトに共有した Mac のフォルダ（`/run/tsugumi/host/`・`/run/tsugumi/chat/`。§3）も、ほかの Card が指定したものやチャットに添付したものを含めて見えます。

### worker の寿命

- 取消・期限切れでは子孫プロセスまで終わらせます（sandbox on / off の両方でテスト）。
- 常駐 worker もプロジェクトの停止・ビルド条件の変更・失敗で終了し、メモリ 2 GiB・常駐数 32 の上限があります。

## 5. Nix 評価の制限

Card の `default.nix` は任意のコードですが、評価には次の制限をかけています（`core/src/nix_eval.cc`、`macOS/Container/vm.nix`）。

- `restrictEval = true`: 許可したパス（Card のソース・SDK・参照する上流定義）以外は読めない。
- `enableImportFromDerivation = false`: 評価中にビルドを走らせない。
- `allowedUris = ["https://github.com/NixOS/nixpkgs/archive/"]`: 取得できるのは nixpkgs のアーカイブだけ。`NIX_PATH` は空。
- ビルドは nix-daemon の sandbox（`sandbox = true`、`build-users-group = nixbld`）で行い、`trusted-users = root` なのでプロジェクトの uid は信頼済みユーザーではありません（substituter や設定を変えられない）。
- substituter は `https://cache.nixos.org` だけで、署名鍵を固定しています。

## 6. RPC の認証

### 待ち受け

- tsugumid は VM 内の `127.0.0.1:<7700 + n>` で待ち受け、Mac 側へは **vsock → `127.0.0.1:<空きポート>` にだけ中継**します（`LoopbackBridge`）。LAN や外部のネットワークには公開しません。
- tsugumid はネットワーク名前空間を分けていないため、VM 内の `127.0.0.1` は全プロジェクトで共通です。**別のプロジェクトの tsugumid に接続すること自体はできますが、token がないので `SessionHello` で拒否されます**。プロジェクト間の RPC の境界は token です（Card はネットワーク名前空間が別なので接続もできません）。
- Unix socket を使う場合は `umask 0177` で作成し `0600` にします。Unix socket ではファイルの権限で接続元を限るため token は任意で、送られた場合だけ照合します（アプリの経路は WebSocket で、常に token 必須）。

### token

- tsugumid は起動ごとに 32 byte の乱数 token を作り（`crypto::random_token(32)`）、**control フォルダの `server.ready`（0600、原子的に書き込み）にだけ**置きます。control フォルダ自体は 0700 です。
- クライアントは最初の `SessionHello` で token を送ります。比較は定数時間（`crypto::equal_secret`）で、一致しなければ `unauthenticated` で拒否します。
- token は URL・ログに出しません（WebSocket の URL は `ws://127.0.0.1:<port>/rpc` だけ）。アプリ内でもメモリに持つだけです。
- 「アプリ設定 > 外部接続」では接続先 URL と `server.ready` のパスをコピーできます。token そのものは画面に表示しません。

### WebSocket

- `GET /rpc` 以外は 404、`Sec-WebSocket-Protocol: tsugumi.rpc.v1` の提示が必須。
- `Origin` ヘッダーがある接続は許可リスト（`--allow-origin`、アプリの経路では空）と照合し、一致しなければ 403。ブラウザは必ず `Origin` を付けるので、**ブラウザ上のページから `127.0.0.1` の Tsugumi へは接続できません**（DNS rebinding・CSRF 型の攻撃の防止）。`Origin` のない接続（アプリ・Agent・テストのようなネイティブクライアント）は `--allow-missing-origin` で受け付け、token で認証します。
- マスクされていないクライアントフレーム、予約ビット、上限を超えるフレーム、未知の union・不正な長さの FlatBuffers は接続を切ります（`test_transport.py`・`test_contract.py`）。

### ページング cursor

履歴のページング cursor には起動ごとの秘密を鍵にした署名（`sha256(secret + payload)` の先頭 32 桁）を付けており、クライアントが cursor を偽造して範囲外を読むことはできません。

## 7. ファイルの取り込み（import grant）

別のプロジェクトから Card 定義を取り込むとき、core に任意のパスを読ませないための仕組みです。

1. ユーザーがアプリで取り込み元を選ぶと、アプリがその時点の内容を control フォルダの `grants/<uuid>/` にコピーし、`grants.json` に登録します。
2. RPC の `Import` 要求は grant の ID だけを指定でき、core は `grants.json` に登録された場所しか読みません。
3. `grants.json` は RPC からは変更できません（アプリだけが書く）。Card からは control フォルダごと見えません。

## 8. モデルの API キー（Agent）

- API キーは **macOS Keychain** に保存します（`agent/Sources/TsugumiAgent/Provider.swift`）。
  - service `app.tsugumi.agent`、account は provider 名（`Anthropic` / `OpenAI`）、1 provider 1 項目。
  - `kSecAttrAccessibleWhenUnlockedThisDeviceOnly`: Mac のロック解除中だけ読め、iCloud キーチェーンやバックアップで他の端末に移りません。
- キーは **リクエストのたびに Keychain から読み、HTTP ヘッダー（`x-api-key` / `Authorization: Bearer`）にだけ載せます**。
- キーは tsugumid・VM・Card worker には渡しません。プロジェクトファイル・アプリ設定・ログ・会話履歴にも書きません。
- HTTP には `URLSessionConfiguration.ephemeral` を使い、ディスクのキャッシュ・cookie を残しません。
- 設定画面ではキーを入力して保存・削除するだけで、保存済みのキーを表示する機能はありません。
- テスト用のローカル fixture（`127.0.0.1`）は Keychain を使いません。

### モデルに送られるもの

Agent はモデルに、会話・選んだ Card などの参照・tool の結果（RPC で読んだ Card 定義・ソース・ログ・値、チャットに添付したフォルダから `AttachedFolderRead` で読んだファイルの内容など）を送ります。**プロジェクトの内容と添付したフォルダの内容の一部は provider に送信される**ことに注意してください。秘密を含むプロジェクトやフォルダで Agent を使う場合は、その内容が provider に渡ってよいかを確認してください。
Agent を使わず、`network: true` の Card もなければ、プロジェクトの内容は Mac と VM の外に出ません（Nix の取得通信を除く）。`network: true` の Card は、自分が読めるもの（入力・指定した Mac のフォルダなど）を外部に送れます。

### Agent の権限

- Agent が使える操作は RPC の tool だけで、UI や Mac のファイルを直接操作しません。core から見れば UI と同じ 1 クライアントで、同じ revision・競合検査を通ります。
- 変更を伴う tool には `op_id` が付き、応答を失っても `OperationGet` で照合し、盲目的に再送しません。
- Agent の取消はモデル通信と未送信の tool を止めます。すでに受理された run は止めません（Workflow の停止は別操作）。
- 会話と tool の証跡はホスト側（`~/Library/Application Support` 配下）に保存されます。

## 9. データの置き場所と削除

| データ | 場所 | 削除方法 |
| --- | --- | --- |
| プロジェクトの文書・ソース | プロジェクトフォルダ | Finder など（Tsugumi は消さない） |
| 実行履歴（正本） | VM の実行履歴ディスク `vm/history.img`（VM 内の `/history-disk/<id>/`） | 「実行履歴を削除」、アプリ設定の「VM 内の実行履歴」 |
| 実行履歴（コピー） | `.tsugumi/runs.sqlite`・`.tsugumi/runs/` | 「実行履歴を削除」、`saveRunHistory = false` で書き出し停止 |
| Nix store・ビルド root | `~/Library/Application Support/Tsugumi/vm/store.img` | 「ビルドキャッシュを削除」「共有 store を掃除」 |
| control フォルダ（token・grant） | `~/Library/Application Support/Tsugumi/environments/<id>/control/` | 「すべての実行環境を初期化」 |
| API キー | macOS Keychain | 設定 > API の削除 |
| 会話 | `~/Library/Application Support` 配下 | — |

「すべての実行環境を初期化」は VM を止めて `vm/`（実行履歴ディスク `vm/history.img` を除く）と `environments/` を削除します。プロジェクトの文書・成果物・`.tsugumi/runs.sqlite`、VM 内の実行履歴には触れません。

## 10. 既知の制限

- **共通 VM は 1 つの Linux カーネルを共有します**。プロジェクト間の隔離は uid・名前空間・cgroup によるもので、カーネルの脆弱性を突かれた場合は他のプロジェクトに及びえます。強い隔離が必要なプロジェクトは同時に開かないでください。
- 共通の `/nix/store` は全プロジェクトから読めます（§3）。
- VM には NAT でのネットワークがあります。tsugumid や nix-daemon は外部と通信できます。Card は既定ではできませんが、`network: true` の Card はできます。
- `network: true` と `hostFolders`（特に `write: true`）は Experimental で、Card に Mac のファイルや外部への通信を許します。どちらもプロジェクトのファイル（`_card.json`）に書かれていれば有効になり、プロジェクトを開くときや実行するときに確認は出ません。他人から受け取ったプロジェクトや AI が生成・変更した Card では、実行する前に Card 設定の Experimental を確認してください。
- `sandbox: false` の Card はプロジェクトの uid で VM 内のファイル（そのプロジェクトに共有した Mac のフォルダを含む）にアクセスできます（control と `.tsugumi` は隠す）。自分で書いたか内容を確認した Card にだけ使ってください。
- 開発用に Mac 上で直接動かす tsugumid（bubblewrap なし）は隔離されません。アプリの実行経路では使いません。
- アプリはローカル用の ad-hoc 署名で、公証はしていません。
