# セキュリティと隔離

この文書では、Tsugumi が設ける「4 段階の信頼境界（どこからどこまでを隔離しているか）」と、「秘密情報（外部モデルの API キーや RPC の認証トークン）の扱い」をまとめています。
すべての記述は現在の実装（`core/src/vmd_main.cc`・`core/src/worker_proc.cc`・`core/src/server.cc`・`macOS/Sources/Launcher/`・`agent/Sources/TsugumiAgent/Provider.swift`）に基づいています。

## 1. 信頼境界の全体像

```text
┌─ macOS（ユーザー権限） ────────────────────────────────────────────────┐
│  Tsugumi.app                                                           │
│   ├ SwiftUI UI                                                         │
│   ├ Agent ── HTTPS ──▶ モデル provider（API キーは Keychain から）     │
│   └ LoopbackBridge 127.0.0.1:<空きポート>  ◀── 外部クライアント        │
│        │ vsock                                                         │
│  ┌─ 共通 Linux VM（Virtualization.framework） ──────────────────────┐  │
│  │  tsugumi-vmd（root、VM の管理役）  nix-daemon（共通 store）      │  │
│  │  ┌─ project A ────────────────────────┐ ┌─ project B ────────┐   │  │
│  │  │ 専用 uid / mount・PID・IPC・UTS ns │ │ 同左（別 uid）     │   │  │
│  │  │ cgroup tsugumi/<A>                 │ │                    │   │  │
│  │  │ tsugumid                           │ │ tsugumid           │   │  │
│  │  │  └ worker（bubblewrap: 全 ns 分離、│ │  └ worker          │   │  │
│  │  │     デフォルトで通信・cap なし）   │ │                    │   │  │
│  │  └────────────────────────────────────┘ └────────────────────┘   │  │
│  └──────────────────────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────────────────────┘
```

隔離の境界は、最も内側から外側へ向かって 4 段あります。

| 境界 | 守る対象 | 仕組み |
| --- | --- | --- |
| Card（worker）⇄ tsugumid | プロジェクト全体・認証 token・他の Card の作業領域・外部ネットワーク（デフォルト） | bubblewrap による全名前空間分離と capability 破棄 |
| project ⇄ project | 他プロジェクトのファイル・プロセス・実行履歴・ビルド root | 専用 uid・名前空間・cgroup（`tsugumi-vmd` が管理） |
| VM ⇄ macOS | Mac 側のファイル全体・ホームフォルダ | `Virtualization.framework`。共有するのは開いているプロジェクトのフォルダと、Card 定義やチャットで指定されたフォルダ（§2）のみ |
| macOS 上の RPC ⇄ 他のプロセス・ネットワーク | core の操作権限 | `127.0.0.1` 限定の待ち受け、token 認証、WebSocket の `Origin` 検査 |

## 2. VM ⇄ macOS

### VM に渡しているもの

| デバイス | 内容 | 書込 |
| --- | --- | --- |
| rootfs | アプリ同梱の erofs イメージ（Linux カーネル 6.12.115 と Nix、`macOS/Container/vm-boot.nix`）。Tsugumi 自身は同梱のソースから VM の中で Nix がビルド（`macOS/Container/vm-system.nix`、依存は署名を検証して cache.nixos.org から取得） | 読み取り専用（ソースの共有も読み取り専用） |
| store ディスク | `~/Library/Application Support/Tsugumi/vm/store.img`（全プロジェクト共通の Nix store。ソースからビルドした開発版は `Tsugumi-dev/` の下。以下同じ） | 可 |
| 実行履歴ディスク | `~/Library/Application Support/Tsugumi/vm/history.img`（全プロジェクトの実行履歴、プロジェクトごとにディレクトリ分離） | 可 |
| virtiofs（スロットごと） | 開いているプロジェクトのフォルダ（`project`）と、その control フォルダ（`control`） | 可 |
| virtiofs（同じスロット、指定時のみ） | Card 設定で指定した Mac のフォルダ（`host-<tag>`）、チャットに添付した Mac のフォルダ（`chat-<tag>`） | `host-` は `write: true` のもののみ可、`chat-` は読み取り専用 |
| vsock | 制御チャネル（1024）と RPC 中継（1025） | — |
| ネットワーク | NAT（`VZNATNetworkDeviceAttachment`） | — |

- **Mac 側のファイルで VM から見えるのは、現在開いているプロジェクトのフォルダとその control フォルダ、およびそのプロジェクトの Card 定義やチャットで明示的に指定されたフォルダだけ**です。ホームフォルダ全体や他のプロジェクトは共有されません。プロジェクトを閉じると、そのスロットの共有は即座に解除されます。
- Mac のフォルダを VM に共有する経路は次の 2 つです（`macOS/Sources/Launcher/EnvironmentManager.swift`・`HostFolders.swift`）。
  - **Card の「ホストのフォルダへのアクセスを許可する」**（`_card.json` の `hostFolders`、Experimental）：プロジェクト（およびその `externals`）の Card 定義に記述されたフォルダを、環境の起動時にそのプロジェクトのスロットへ共有します。書き込みを許可したもの（`write: true`）以外は読み取り専用です。**この共有は定義ファイルの記述だけで決まり、プロジェクトを開く際に確認ダイアログは出ません**。外部から受け取ったプロジェクトや Agent が変更した Card 定義に含まれている場合も同様に共有されるため、注意が必要です（§10）。
  - **チャットへのフォルダ添付**：Agent が参照するためのフォルダを、そのプロジェクトに対して読み取り専用で共有します。
- 共有にはプロジェクトごとに独立した virtiofs デバイス（`tsugumi-<slot>`）を割り当てるため、あるプロジェクトの共有を差し替えても他のプロジェクトには影響しません。
- アプリに付与している entitlement は `com.apple.security.virtualization` のみです（`macOS/Resources/Tsugumi.entitlements`）。
- VM は `vm/vm.lock` のファイルロック（`flock`）を取得した 1 つのアプリプロセスだけが起動します。

### VM のネットワーク

VM の NAT は、**`nix-daemon` が `cache.nixos.org` からバイナリキャッシュを取得し、固定された nixpkgs アーカイブをダウンロードするため**に存在します。
Card 自身はデフォルトでネットワーク名前空間から切り離されており、外部と通信できません。Card 設定で「ネットワークへのアクセスを許可する」（`_card.json` の `network: true`、Experimental）を有効にした Card だけが、この NAT を通じて外部と通信できます（§4）。

## 3. project ⇄ project（VM 内）

VM 内の管理プロセス `tsugumi-vmd` が、プロジェクトごとに次の隔離条件で `tsugumid` を起動します（`core/src/vmd_main.cc`）。

- **専用 uid**：プロジェクトごとに固有の uid（20001 以降、`/persist/tsugumi/uids` に記録）を割り当て、`setgroups(0)` → `setgid` → `setuid` で root 権限を完全に放棄してから起動します。
- **名前空間**：`CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWIPC | CLONE_NEWUTS` を分離します。
- **見えるファイル**：mount 名前空間内で以下のパスだけを bind mount し、全スロットの親ディレクトリ（`/mnt`）、store ディスク全体（`/persist`）、全プロジェクトの実行履歴ディスク全体（`/history-disk`）はすべて `umount` します。
  - `/project` … 自身のプロジェクトフォルダ
  - `/control` … 自身の control フォルダ
  - `/history` … 自身の実行履歴フォルダ（実行履歴ディスク上、`nosuid,nodev`）
  - 専用の `/tmp` と `/run/tsugumi`（`tmpfs`）
  - `/run/tsugumi/rpc` … `tsugumid` の待ち受けソケット（WebSocket、Unix ソケット）。実体は `/run/tsugumi-rpc/<id>`（そのプロジェクトの uid だけが入れる 0700 のフォルダ）で、`tsugumi-vmd` がここへ中継します。VM の loopback にはどのプロジェクトのポートもありません（ネットワークを許可した Card は VM のネットワークを共有するため、ポートがあれば他のプロジェクトの `tsugumid` に届いてしまいます）。
  - `/run/tsugumi/host/<tag>`・`/run/tsugumi/chat/<tag>` … そのプロジェクトに共有された Mac のフォルダ（§2、指定時のみ）
  - `/nix` … オーバーレイマウント（共通 store は全プロジェクトから読み取り可能、書き込みは `nix-daemon` 経由のみ）
- **cgroup**：`/sys/fs/cgroup/tsugumi/<id>` に配置します。プロジェクトを停止する際は `cgroup.kill` によってその cgroup 内のプロセスだけを確実に終了させ、他のプロジェクトには一切影響を与えません。
- **GC root**：`/nix/var/nix/gcroots/tsugumi/projects/<id>/` は `tsugumi-vmd` が root 権限で作成し、親ディレクトリにはプロジェクト側からの書き込み権限を与えません。そのため、他のプロジェクトが先にディレクトリを作って横取りすることはできません（`test_shared_vm.py` で検証済み）。
- **カーネル**：Linux 6.12 LTS（`macOS/Container/kernel.nix`）から io_uring を外し、非特権 BPF を既定で無効にしています。`/init` が sysctl でカーネルアドレスとログの読み取り、ptrace（子孫のみ）、共有の sticky フォルダでのリンク・ファイルの差し込みを制限します（Yama を有効にしています。BPF JIT と userfaultfd は組み込んでいません）。
- **ターミナル**：VM 内ターミナルを開く際は、対象プロジェクトの名前空間に `setns` で入り、同じ cgroup に所属させ、プロジェクトの uid に権限を落としてから `bash` を起動します。プロジェクトを閉じればターミナルも終了します。

### プロジェクト間で共有しているもの

- `nix-daemon` と `/nix/store` を共有しています。store 内のパスはすべて入力のハッシュで識別され、ビルドは `nix-daemon` のサンドボックス（`sandbox = true`）内で行われるため、あるプロジェクトが他プロジェクトのビルド成果物を書き換えることはできません。ただし、Nix の仕様上、**「どのパスが store 内に存在するか」は全プロジェクトから参照可能**です。秘密情報を Nix store に入れないでください。

## 4. Card（worker）⇄ tsugumid

各 Card のコードは、さらに `bubblewrap` のサンドボックス内で実行されます（`core/src/worker_proc.cc`）。

### `sandbox: true`（デフォルト）

```text
bwrap --unshare-all --die-with-parent --new-session --cap-drop ALL
      --ro-bind /nix/store /nix/store --proc /proc --dev /dev --tmpfs /tmp
      --symlink <store の bash> /bin/sh
      --uid <nobody 相当> --gid <…>
      --ro-bind <保存済み入力・runs>  --bind <worker の一時領域>
      -- tsugumi-worker
```

- **すべての名前空間を分離**（user・mount・PID・network・IPC・UTS・cgroup）します。ネットワークインターフェースはループバック（`lo`）のみです。`network: true` を指定した Card に限り、network 名前空間を分離せず（`--share-net`）、名前解決設定と CA 証明書ファイルを読み取り専用でマウントします。
- **Linux capability をすべて破棄**（`--cap-drop ALL`）します。
- **見えるファイル**：`/nix/store`（読み取り専用）、プロジェクトの `runs` と保存済み入力（読み取り専用）、その worker 専用の一時作業領域（書き込み可）、およびその Card が `hostFolders` で指定した Mac のフォルダ（`/host/<name>`、`write: true` のもののみ書き込み可）だけに限定されます。プロジェクトの定義文書やソースコード、control フォルダ、認証 token、他の Card の作業領域、他の Card が指定したホストフォルダ、チャットに添付されたフォルダは一切見えません。
- **環境変数は空から再構成**（`PATH=/var/empty`、`LANG=C.UTF-8`、および Tsugumi が渡す変数のみ）し、`tsugumid` やホストの環境変数は引き継ぎません。
- **出力の受け渡し**：各呼び出しの出力先ディレクトリは `SCM_RIGHTS` でファイル記述子（FD）として渡し、worker 内からは `/proc/self/fd/900` としてアクセスさせます。実行後に core が本来のパスへ戻して検証します。
- **入力を FD で渡さない理由**：読み取り専用で開いた FD であっても、Linux では `/proc/self/fd/N` を経由して開き直すと書き込みが可能になるケースがあることを実測で確認したためです。そのため、入力は読み取り専用の bind mount で保護しています。
- **`bubblewrap` が利用できない環境では、`sandbox: true` の Card を実行せず即座に拒否します**（`permission_denied`）。暗黙のうちに隔離なしで実行することはありません。
- 例外は [Web UI](WebUI.md) のように `tsugumid` を macOS ホストで直接動かす場合で、bubblewrap の代わりに `sandbox-exec`（Seatbelt）で隔離します（ネットワークなし、読み取りは `/nix/store`・実行と入力の領域・システムライブラリのみ、書き込みは worker 自身の領域と実行の出力フォルダのみ）。`network: true` の Card にはネットワークを開きますが、このマシン自身のアドレス（loopback・LAN・VPN）と Unix ソケットは閉じたままで、例外は名前解決に必要な `mDNSResponder` のソケットだけです。Mach サービスも、証明書の検証に使う `com.apple.trustd.agent` のほかは閉じているので、クリップボードや LaunchServices には届きません。`sandbox-exec` がない場合は同じく拒否します。

### `sandbox: false`

ファイルシステム参照の制限だけを緩和する設定ですが、以下の保護は引き続き維持されます。

- user・PID・**network**・IPC の各名前空間は分離されます（`network: true` を併せて指定しない限り外部通信はできず、子孫プロセスも worker 終了時にすべて回収されます）。
- control フォルダと `.tsugumi` ディレクトリは空の `tmpfs` でマスクして隠します。virtiofs の仕様上、すべてのファイルが「参照する側のプロセス所有」として見えるため、通常のファイルパーミッション（mode）だけでは認証 token を保護できないためです。
- VM 内のファイルにはプロジェクトの uid でアクセスできるため、そのプロジェクトに共有された Mac のフォルダ（`/run/tsugumi/host/`・`/run/tsugumi/chat/`、§3）も、他の Card が指定したものやチャットに添付されたものを含めて参照可能になります。

### worker の寿命管理

- キャンセルやタイムアウト期限切れの際は、worker が起動した子孫プロセスまで残さず終了させます（sandbox の on / off 両方で自動テスト済み）。
- 常駐 worker も、プロジェクトの停止・ビルド条件の変更・実行失敗時には即座に破棄され、メモリ使用量（上限 2 GiB）と常駐プロセス数（上限 32）が制限されています。

### Card のグラフ（Plotly・Vega-Lite）の表示

Card が送る Plotly の図と Vega-Lite の仕様は信頼しないデータとして扱い、同梱の plotly.js / Vega で描く表示部品（`ui/modules`）の中だけで動かします。

- Web UI: `/modules/viewer.html` を iframe に入れます。応答の CSP は `sandbox allow-scripts`（`allow-same-origin` なし）です。そのため表示部品は opaque origin で動き、cookie も `/api/` も使えません。`default-src 'none'`・`connect-src 'none'` なので何も読み込まず、どこにも接続しません。スクリプトはページに埋め込み、ライブラリのコードは親ページが postMessage で渡します。埋め込みを許すのはこの経路だけ（`frame-ancestors 'self'`）で、ほかの応答は従来どおり埋め込みを禁じます。
- macOS アプリ: 表示部品を WKWebView（保存しないデータストア）で開き、content rule で `file:` 以外の読み込みをすべて遮断します。
- どちらでも、Vega のデータ読み込み（`data.url`）は loader で拒否します。グラフのデータは仕様に含めるよう SDK が送ります。

## 5. Nix 評価の制限

Card の `default.nix` には任意の Nix 式を書けますが、評価時には以下の制限を強制しています（`core/src/nix_eval.cc`、`macOS/Container/vm-boot.nix`）。

- `restrictEval = true`：許可されたパス（Card のソース・SDK・参照が許可された上流定義）以外のファイルを読み取れません。
- `enableImportFromDerivation = false`：Nix 式の評価中にビルドやコマンド実行を走らせる（IFD）ことを禁止しています。
- `allowedUris = ["https://github.com/NixOS/nixpkgs/archive/"]`：ネットワーク経由で取得できる URL を公式 nixpkgs のアーカイブのみに限定しています（`NIX_PATH` も空です）。
- ビルドは `nix-daemon` のサンドボックス（`sandbox = true`、`build-users-group = nixbld`）内で行われ、`trusted-users = root` のみとしているため、プロジェクト側の uid から substituter や Nix 設定を書き換えることはできません。
- バイナリキャッシュの取得先は `https://cache.nixos.org` のみとし、署名検証鍵を固定しています。
- **固定出力 derivation（`fetchurl` など、内容を hash で固定したもの）は、ビルドのときに外部と通信できます**。これは Nix の仕様で、Card の `network` 設定とは別です（`network: false` の Card の `default.nix` でもデータを取得できます）。取得したものは hash と一致しなければ捨てられますが、取得先への通信そのもの（URL に含めた値など）は外へ出ます。

## 6. RPC の認証

### 待ち受け経路

- `tsugumid` は VM の中では TCP ポートを開かず、Unix ソケット `/run/tsugumi/rpc/s` で WebSocket を待ち受けます（`--listen-unix-ws`、`core/src/vmd_main.cc`）。実体はそのプロジェクトの uid だけが入れる 0700 のフォルダ `/run/tsugumi-rpc/<id>` にあります（§3）。
- Mac 側へは **vsock を介してホストの `127.0.0.1:<空きポート>` にのみ中継**されます（`LoopbackBridge`、vsock 1025 で `tsugumi-vmd` がプロジェクト ID からソケットへつなぐ）。LAN や外部ネットワークには一切公開されません。
- VM の loopback にポートがないので、別のプロジェクトや、ネットワークを許可した Card から `tsugumid` へ接続する経路はありません。
- Web UI では `tsugumid` がホストで直接動き、`127.0.0.1` の WebSocket で待ち受けます（[Web UI](WebUI.md)）。
- Unix ドメインソケットを使う場合は `umask 0177`（パーミッション `0600`）で作成します。アプリの標準経路は WebSocket であり、常に token 認証が必須です。

### 認証 token

- `tsugumid` は起動ごとに 32 byte の暗号論的乱数 token を生成し（`crypto::random_token(32)`）、**control フォルダ内の `server.ready`（パーミッション `0600`、原子的書き込み）にのみ**出力します。control フォルダ自体も `0700` で保護されています。
- クライアントは最初の `SessionHello` メッセージで token を提示します。照合には定数時間比較（`crypto::equal_secret`）を用い、一致しなければ `unauthenticated` で切断します。
- token は URL やログには一切出力されず（WebSocket URL は `ws://127.0.0.1:<port>/rpc` のみ）、アプリ内でもメモリ上にしか保持しません。
- 設定の「外部接続」では接続先 URL と `server.ready` のファイルパスをコピーできますが、token の文字列そのものは画面に表示しません。

### WebSocket の保護

- エンドポイントは `GET /rpc` のみを受け付け（それ以外は 404）、`Sec-WebSocket-Protocol: tsugumi.rpc.v1` の提示を必須としています。
- `Origin` ヘッダーが付与された接続は許可リスト（`--allow-origin`、アプリ起動時は空）と照合し、一致しなければ 403 で拒否します。一般的な Web ブラウザからの接続には必ず `Origin` ヘッダーが付くため、**悪意ある Web ページを開いたブラウザから `127.0.0.1` の Tsugumi へ接続することはできません**（DNS rebinding や Cross-Site WebSocket Hijacking の防止）。`Origin` ヘッダーを持たないネイティブクライアント（アプリ本体・Agent・テストスクリプト）は token によって認証します。
- マスクされていないクライアントフレーム、予約ビットが立ったフレーム、サイズ上限を超えるフレーム、未知の union 型や不正な長さを持つ FlatBuffers メッセージを受信した場合は即座に接続を切断します（`test_transport.py`・`test_contract.py` で検証済み）。

### ページング cursor の署名

履歴取得などのページング cursor には、起動ごとに生成される秘密鍵を用いた HMAC 相当の署名（`sha256(secret + payload)` の先頭 32 桁）を付与しており、クライアント側で cursor を改ざんして範囲外のデータを読み取ることはできません。

## 7. ファイルの取り込み（import grant）

他のプロジェクトから Card 定義を取り込む際、RPC クライアント（Agent を含む）がホスト上の任意のファイルパスを `tsugumid` に読ませることを防ぐ仕組みです。

1. 利用者がアプリのファイル選択ダイアログで取り込み元を選ぶと、アプリがその時点の内容を control フォルダ内の `grants/<uuid>/` にコピーし、`grants.json` に登録します。
2. RPC の `Import` リクエストではこの grant ID だけを指定でき、`tsugumid` は `grants.json` に登録されたパス以外を一切読み取りません。
3. `grants.json` は RPC からは変更できず、アプリ本体だけが書き込めます。また Card の worker からは control フォルダ自体が見えません。

## 8. モデルの API キー（Agent）

- LLM プロバイダの API キーは利用者の **ログインキーチェーン**（macOS Keychain）に保存します（`agent/Sources/TsugumiAgent/Provider.swift`）。
  - サービス名は `app.tsugumi.agent`、アカウント名はプロバイダ名（`Anthropic` / `OpenAI`）です。アプリと Web UI のサーバーは同じ項目を使うので、どちらで登録したキーも両方で使えます。
  - ログインキーチェーンの項目は iCloud キーチェーンに同期されません。読めるのはログインキーチェーンのロックが解除されている間（通常はログイン中）です。
  - 項目のアクセスリストは、保存したプログラムだけを信頼します。アプリで保存したキーをほかのプログラム（`security` コマンドを含む）が読もうとすると、macOS が利用者に確認します。ソースからビルドしたアプリは ad-hoc 署名のため、ビルドし直すと最初の読み出しで一度確認が出ます。
  - Web UI のサーバーは `/usr/bin/security` を通して読み書きします。Web UI で保存したキーは `security` を信頼するため、この利用者として動くプログラムなら確認なしに `security find-generic-password` で読めます。両方を使う場合は、アプリの設定からキーを登録することを勧めます。macOS 以外（または Keychain に保存できなかったとき）は、Web UI のデータフォルダのファイル（0600）に保存します。
  - データ保護キーチェーン（`kSecUseDataProtectionKeychain`）は使いません。これには provisioning profile でしか与えられない entitlement が必要で、ad-hoc 署名のビルドでは使えないためです（`errSecMissingEntitlement`）。
- API キーは **モデルへのリクエスト送信時に Keychain から読み出し、HTTPS のリクエストヘッダー（`x-api-key` / `Authorization: Bearer`）にのみ載せます**。Web UI のサーバーは読み出したキーをそのプロセスのメモリに保ち（項目の属性が変わると読み直します）、拒否された読み出しも覚えるので、`security` の確認が同じキーで何度も出ることはありません。
- チャットのモデルメニューは、開くたびに設定済みの接続のモデル一覧を取得し直します。利用者が頼んだ読み出しではないので、アプリは確認の画面を出さずに読めるときだけ読み（`Keychain.loadWithoutAsking`）、読めない接続は保存済みの一覧のまま次の機会を待ちます（チャットの実行や設定の「モデル一覧を取得」では、これまでどおり確認が出ます）。アクセストークンの更新が必要なときだけは、更新後のリフレッシュトークンを必ず保存するため、確認なしで読めたあとの保存を止めません。Web UI のサーバーでは `security` に確認なしで読む手段がないため、まだ読んでいないキーについて、サーバーの起動ごとに最大 1 回確認が出ることがあります。
- API キーを `tsugumid`・Linux VM・Card worker へ渡すことはありません。また、プロジェクトファイル・アプリ設定・ログ・会話履歴にも一切記録しません。
- HTTPS 通信には `URLSessionConfiguration.ephemeral` を使用し、ディスク上にキャッシュや Cookie を残しません。
- 設定画面ではキーの登録と削除のみを行え、登録済みのキーを画面に再表示する機能はありません。

### ChatGPT プラン（Sign in with ChatGPT）

API キーの代わりに、利用者の ChatGPT（Plus・Pro）プランで Agent を使えます。OpenAI が[オープンソースのツール向けに公開している手順](https://developers.openai.com/siwc/token-sharing-open-source)（ChatGPT plan usage）に従っています（`agent/Sources/TsugumiAgent/ChatGPT.swift`、Web UI は `web/server/chatgpt.ts`）。

- サインインは既定のブラウザで行います。Tsugumi は `127.0.0.1` の空いたポートで `/auth/callback` を待ち、結果を受け取るとすぐに閉じます。毎回新しい `state`・`nonce`・PKCE（S256）を使い、`state` が合わない応答は捨てます。client secret や開発者の API キーは使いません（初回は `dynamic_agent_client` で Tsugumi を登録し、発行された `client_id` を以後のサインインに使います）。
- ID トークンは OpenAI の JWKS（`https://auth.openai.com/.well-known/jwks.json`）で RS256 の署名を確かめ、発行者・audience（発行された `client_id`）・期限・`nonce` を検査します。以前のアカウントに入り直すときは、同じアカウント（`sub`）であることも確かめます。プランを使う許可（`chatgpt.tokens.use.direct`）が付与されなかったときは、Agent はプランを使わず、許可し直すか API キーを使うよう案内します。
- ホスト ID（`ext_agent_host_id`）は最初のサインインの前に作る `urn:uuid:`（UUIDv4）で、利用者を識別する情報を含みません。
- アプリでは、ホスト ID・登録（アカウント）ごとの `client_id`・メールアドレス・ID / アクセス / リフレッシュトークンを、API キーと同じサービスの **ログインキーチェーン** の 1 項目（アカウント名 `ChatGPT`）に置きます。開発版とリリース版は同じ項目を使い、トークンの更新（リフレッシュトークンは更新のたびに替わります）は `~/Library/Caches/app.tsugumi.agent/ChatGPT.lock` で 1 つずつ行い、更新の前に項目を読み直します。
- Web UI のサーバーは別の agent host です。`security -i` では長い記録を書けないため、ホスト ID と登録・トークンをデータフォルダの `chatgpt.json`（0600、書き換えは一時ファイルから rename）に置きます。アプリでサインインしていても、Web UI では別にサインインします。サインインはサーバーと同じマシンのブラウザで行ってください（`127.0.0.1` の受け口に届く必要があります）。
- アクセストークン（1 時間）は期限の 5 分前から更新し、API に拒まれた（401）ときは 1 度だけ更新して送り直します。更新が `invalid_grant` などで終わったセッションのトークンは消し、同じ登録でのサインインを求めます。通信やサーバーの一時的な失敗ではトークンを消しません。
- サインアウトは OpenAI の取り消し口（revocation endpoint）でリフレッシュトークンを取り消してから、手元のトークンを消します（登録とホスト ID は残ります）。取り消しを確かめられなかったときは、ChatGPT の設定から Tsugumi の接続を解除できることを伝えます。
- トークンは API キーと同じく、`tsugumid`・Linux VM・Card worker・ページへ渡さず、プロジェクトファイル・設定・ログ・会話履歴にも記録しません。リクエストのヘッダー（`Authorization: Bearer`）にだけ載せ、送り先は `https://api.openai.com/v1/responses` と `/v1/models` だけです。
- プランでのリクエストは `store: false`・`stream: true` で送り（会話は OpenAI 側に保存されません）、`max_output_tokens` を付けず、tool は名前空間 `tsugumi` にまとめます。利用上限（`subscription_sharing_usage_limit_exceeded`）では再試行せずに止め、チャットに「Manage usage」（`https://chatgpt.com/settings/usage`）を出します。別の課金方法へ黙って切り替えることはありません。Plus の 5 時間ごとの上限は、ほかのアプリと共有です。
- 吹き出しの「思考の過程」の要約は、プランの利用枠を使わないよう、ChatGPT プランでは作りません（決まった文だけを出します）。

### モデルに送られるもの

Agent を使う際、接続先のモデルプロバイダ（Anthropic / OpenAI。ChatGPT プランも OpenAI）には以下の情報が送信されます。
- チャットの会話本文
- チャットに参照として追加した Card や可視化の情報
- Agent が tool（RPC）を通じて読み取った内容（Card の定義・ソースコード・実行ログ・入出力値、およびチャットに添付したフォルダから `AttachedFolderRead` で読み取ったファイル内容など）
- ガイドのツグミに質問したとき、またデモの案内中：質問文と、そのとき画面に出ているものの要約（プロジェクトの名前とフォルダ、表示中の Workflow の Card、選択した Card の状態と結果の要約、実行と接続の状態、同梱デモの一覧）、およびガイドが読み取りの tool で読んだ内容。ガイドは保存・実行・停止の tool を持たず、プロジェクトを変えません。

**Agent を使用すると、プロジェクト内のコードやログ、入出力データ、添付フォルダの内容の一部が外部のモデルプロバイダへ送信される**点に十分ご注意ください。未公開データや機密情報を含むプロジェクトで Agent を使う場合は、それらが接続先プロバイダへ送信されてよいかをあらかじめ確認してください。

なお、**Agent を使用せず、かつ `network: true` を有効にした Card も存在しない場合、プロジェクトの内容やデータが Mac とローカル VM の外へ出ることはありません**（Nix がパッケージを取得する通信を除く）。`network: true` を許可した Card は、その Card がアクセスできるデータ（入力値や指定したホストフォルダの内容など）を外部ネットワークへ送信できます。

### Agent の権限境界

- Agent が実行できる操作は公開された RPC の tool のみであり、UI や Mac のファイルシステムを直接操作することはできません。`tsugumid` から見れば UI と対等な 1 クライアントであり、同じリビジョン検査と競合検出を通ります。
- 変更を伴う tool 呼び出しには一意な `op_id` が付与され、応答が途切れた場合も `OperationGet` で適用状態を確認するため、盲目的な再実行による二重適用は起きません。
- **Card の権限は利用者のものです**。Agent の保存が Card にネットワーク（`network: true`）、隔離なしの実行（`sandbox: false`）、ホストのフォルダ（`hostFolders`、読み取りと書き込み）を新たに与えるとき、`tsugumid` は保存せず `approval_required` で返し、広がる権限を 1 つずつ挙げます（Card in Card の中の Card も含みます。`core/src/permissions.cc`）。アプリ（Mac・Web）は、設定の「API」で「Agent が確認なしに Card に与えてよい権限」に入れた種類はそのまま承認し、それ以外は利用者に確認します。確認の画面で、その種類を以後は確認しないことにもできます（既定ではすべて確認します）。承認は Agent の実行部分がつける欄（`approved_permissions`）で送り、モデルからは設定できません。許可されなければ何も保存されず、Agent には理由を利用者に説明するよう伝えます。利用者自身が Card の設定で変えるときは確認しません。Canvas の Card には、持っている権限がアイコンで表示されます。
- **Claude Code の plugin**（[Claude Code から使う](claude-code.md)）は、Web UI のサーバーを通してだけ操作します。サーバーはチャットの Agent と同じ道具を同じ検査で実行し、Card の権限を広げる保存は Web UI のページで利用者の承認を待ちます（ページが開いていなければ保存しません）。チャットに添えたフォルダの読み取りは渡しません。plugin の MCP サーバーは、サーバーがデータフォルダに書く `server.json`（ポートと起動ごとの鍵、0600、終了時に削除）でサーバーを見つけます。この利用者として動くプログラムは、この鍵で Web UI の `/api/` を使えます（起動時に表示する URL の鍵と同じものです）。
- チャットの「停止」はモデルとの通信および未送信の tool 呼び出しを止めます（すでに開始された Workflow の実行自体を止める場合は、Canvas の停止ボタンを使います）。
- 会話履歴と tool の呼び出し証跡はホスト側（`~/Library/Application Support` 配下）に保存されます。

## 9. データの置き場所と削除方法

| データ | 保存場所 | 削除・管理方法 |
| --- | --- | --- |
| プロジェクトの文書・ソース | プロジェクトフォルダ | Finder 等で管理（Tsugumi が勝手にフォルダ自体を消すことはありません） |
| 実行履歴（正本） | VM の実行履歴ディスク `vm/history.img`（VM 内の `/history-disk/<id>/`） | プロジェクト設定の「実行履歴を削除」、または設定の「VM 内の実行履歴」 |
| 実行履歴（プロジェクト内コピー） | `.tsugumi/runs.sqlite`・`.tsugumi/runs/` | 「実行履歴を削除」、または `saveRunHistory = false` で書き出しを停止 |
| Nix store・ビルド root | `~/Library/Application Support/Tsugumi/vm/store.img` | 「ビルドキャッシュを削除」および「共有 store を掃除」 |
| control フォルダ（token・grant） | `~/Library/Application Support/Tsugumi/environments/<id>/control/` | 「すべての実行環境を初期化」 |
| API キー | macOS Keychain | 設定 > API から削除 |
| チャットの会話履歴 | `~/Library/Application Support` 配下 | — |

設定の「すべての実行環境を初期化」を実行すると、VM を停止したうえで `vm/`（実行履歴ディスク `vm/history.img` を除く）と `environments/` を削除します。プロジェクトフォルダ内の文書・成果物・`.tsugumi/runs.sqlite`、および VM 内の実行履歴ディスクは保持されます。

## 10. 既知の制限と注意事項

- **共通 VM は 1 つの Linux カーネルを共有しています**。プロジェクト間の隔離は Linux の uid・名前空間・cgroup によるものであり、万一カーネル自体の脆弱性を突かれた場合は他プロジェクトへ影響が及ぶ可能性があります。極めて強い隔離が求められるプロジェクト同士は、同時に開かないでください。
- 共通の `/nix/store` はすべてのプロジェクトから読み取り可能です（§3）。
- VM 自体には NAT による外部ネットワーク接続があります。`tsugumid` や `nix-daemon` は外部と通信可能であり、Card も `network: true` を設定したものは外部と通信できます。
- Mac 上の `tsugumid`（Web UI）で `network: true` の Card は、同じ LAN や VPN の他の機器に届き、UDP のブロードキャストとマルチキャストはこのマシンにも届きます。Seatbelt では宛先のアドレスを指定して拒否できないためです（[Web UI の隔離](WebUI.md#隔離macos-ホスト)）。
- `network: true` と `hostFolders`（特に `write: true`）は Experimental 機能であり、Card に対して外部通信や Mac 側フォルダへのアクセスを許可します。これらはプロジェクト内の `_card.json` に記述されていれば有効になり、プロジェクトを開く際や実行時に確認ダイアログは表示されません（Agent が与えようとする場合は §8 のとおり確認します）。外部から受け取ったプロジェクトを動かす前には、Card のアイコンと Card 設定の Experimental 項目を必ず確認してください。
- `sandbox: false` の Card はプロジェクトの uid で動作するため、VM 内のプロジェクト領域（そのプロジェクトに共有されたすべての Mac 側フォルダを含む）へアクセスできます（control フォルダと `.tsugumi` は隠蔽されます）。内容を確認した信頼できる Card にのみ使用してください。
- 開発・テスト用に Mac ホスト上で直接 `tsugumid` を動かすモード（`bubblewrap` なし）ではサンドボックス隔離が行われません（通常のアプリ実行経路では使用されません）。
- リリース版もソースからビルドしたアプリも ad-hoc 署名であり、Developer ID による署名と Apple による公証（Notarization）はしていません。リリース版は GitHub Actions がこのリポジトリのタグからビルドしたもので（`.github/workflows/release.yml`）、自動更新は GitHub の最新リリースの `Tsugumi.zip` を、同じリリースの `Tsugumi.zip.sha256`・コード署名の検証・bundle id・リリース版であること・版番号を確かめてから入れます。更新の正しさは、このリポジトリとその GitHub Releases に書き込める人を信頼することに依っています（Apple の公証や独自の署名鍵による検証はしていません）。
