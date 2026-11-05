# Tsugumi の主要機能

Tsugumi は、Card をポートで接続したグラフ（Workflow）を Nix で固定した環境で実行し、そのすべての過程と結果を記録する macOS アプリです。
この文書では、Tsugumi の中核機能を「何ができるか」と「どう実現しているか」の両面から解説します。
導入・ビルド手順は [INSTALL.md](install.md)、テスト一覧や計測値は [Development.md](development.md)、隔離境界と秘密情報の扱いは [security.md](security.md)、他ツールとの比較は [novelty.md](novelty.md) を参照してください。

## 1. 全体構成

```text
macOS（ホスト）                                   共通 Linux VM（アプリに 1 台）
 SwiftUI UI ─┐                                     tsugumi-vmd（VM の管理役）
 Agent ──────┼─ FlatBuffers RPC ─ 127.0.0.1 ─ vsock ─ project A: tsugumid ─ worker プール
 外部クライアント┘                                   project B: tsugumid ─ worker プール
                                                   nix-daemon ─ 共通 /nix/store
```

- **tsugumid（core）**：プロジェクトごとに 1 プロセスが立ち上がります。文書の管理・保存、RPC、スケジューラー、Nix 評価、worker 管理、実行履歴の記録を一手に担い、UI がなくても RPC だけで保存・実行・履歴取得が完結します。
- **UI と Agent は対等なクライアント**：どちらも `protocol/tsugumi.fbs` で定義された同一の RPC 契約で core と通信します。Agent は画面の操作を模倣するのではなく、core に直接 RPC リクエストを送ります。Agent を無効にしても通常の操作はすべて機能します。
- **共通 VM**：macOS の `Virtualization.framework` で 1 台の Linux VM を起動し、その内部でプロジェクトごとに名前空間・uid・cgroup を分離した `tsugumid` を動かします（詳細は [security.md](security.md)）。

## 2. Card と Workflow

### Card の種類

| 種類 | 役割 |
| --- | --- |
| コード Card | `default.nix` で実行環境を、`src/` で処理を定義する（C++ / Python / bash） |
| trigger | Workflow の実行起点（出力ポート `out`） |
| input | `_outputs.fbs` の型に沿った入力値を Canvas 上で直接編集して流す（File 型は選択したファイルを run にコピー） |
| fork / branch / merge | 分配・条件分岐（`condition` に応じて `a` / `b` の片側のみ発火）・合流（入力ポート名付きの union 型）。有限のフィードバックループ専用の Card はなく、merge で値を戻して作り、ループ上の Card の `maxRuns` で反復の上限を決めます |
| logger | 受け取った値を外部 DB に記録する（現在は `.tsugumi/logger/` 配下の `sqlite:<name>` に対応） |
| subgraph（Card in Card） | 子 Workflow を 1 つの Card として扱う。`boundary` で外側のポートを内部の Card に対応づける |
| Draft | 名前と内容をカードに直接書き、線を落とすだけでつなげる「未実装の Card」（ポートは線に合わせて作られ、型はない）。後から Agent に実装を生成させられる |

### 型付きのポート

- 出力ポートからの線は 1 本です。1 つの値を複数の Card に送るときは fork Card を通します（同じことを書く方法を 1 つにし、Canvas で分配が Card として見えるようにするため）。2 本以上の線が出ている出力は `fan_out` として報告され、実行できません。Canvas で使用中の出力からもう 1 本引くと、fork が自動で挟まります（出力が fork や fork につながっているときは、その fork に枝が増えます）。
- 出力ポートの型は FlatBuffers スキーマ（`_outputs.fbs`）で宣言し、下流の入力ポートの型は上流から自動的に導出されます。配線時に構造的互換性を検査し、merge では自動的に union 型が構成されます。
- 値は FlatBuffers のバイナリ列のまま Card 間を流れます。C++ / Python / bash の間で 64 bit 整数・enum・struct・union・File / Directory を一切の欠落なく受け渡せることを自動テストで検証しています。

### 実行の意味論（スケジューラー）

- 入力の待ち合わせ方には `all`（すべての必須入力が揃ったら 1 値ずつ消費）と `any`（到着順に消費）があります。
- 有界キューと背圧（バックプレッシャー）、並列度 1〜64、キャンセル、タイムアウト期限（呼び出し全体で 1 つ）、反復上限（`maxRuns`）を備えています。
- 条件分岐で選ばれなかった経路の下流は `skipped`、Draft や失敗した上流に依存する Card は理由付きの `blocked` となり、それらと独立した経路は止まらずに実行されます。
- 異常終了や再起動で途中だった実行は `interrupted` として記録されます。
- 「ここから実行」や「選択 Card のみ」による単体再実行では、前回保存された入力（File の実体を含む）をそのまま再利用します。

## 3. Nix による再現可能な実行環境

### Card ごとの環境定義

```nix
{ pkgs, inputs, params }:
let
  sdk = inputs.tsugumi.sdk { languages = [ "cpp" ]; };
in
{
  run = inputs.tsugumi.cppCard { inherit sdk; src = ./src; };
  preRun = "mkdir -p scratch";
}
```

- `pkgs` はデフォルトで同梱の固定 nixpkgs（`tsugumi:default`）を指します。任意の nixpkgs を `https://github.com/NixOS/nixpkgs/archive/<rev>.tar.gz#sha256-…` の形式でリビジョンとハッシュ付きで固定することもできます。
- Nix の評価には restricted eval・IFD（Import From Derivation）禁止を適用し、取得先 URL を nixpkgs アーカイブのみに制限しています。
- Tsugumi 独自のビルドキャッシュ管理は持たず、「評価した derivation の出力パスが `/nix/store` に存在すればビルドしない」という Nix 本来の仕組みだけで一貫して判定します。

### 上流定義の参照（`inputs.upstream`）

- `default.nix` から、**どの経路を通って発火しても必ず先に実行される**上流 Card の定義（`run`・`preRun`・`exports` の関数や値）を参照できます。
- 「必ず先に実行されるか」の判定は、`all` / `any`・複数経路・ループ（最大不動点）・入れ子構造を考慮した静的解析で行います。分岐の片側にしか存在しない Card や下流、自分自身は参照できず、静的な診断エラー（`nix_reference_*`）となります。`inputs.local.<id>` は同じ階層の Card だけを指します。
- 静的な診断は書かれた参照（`inputs.upstream."<path>"`・`inputs.local.<id>`）を評価前に知らせるためのもので、コメントと文字列の本文（`''…''` の中のシェルスクリプトなど）は読みません（文字列の中の `${…}` は読みます）。別名や `${…}` で組み立てた名前などの間接的な参照は静的には調べず、評価時に判明します。参照できる範囲そのものは、評価器が `inputs.upstream` に必ず先に実行される Card だけを渡すことで保証しています。
- 実際に参照された定義だけが遅延評価され、実行時に選ばれなかった分岐はビルドされません。

### 評価器の組み込み

- `tsugumid` は `libnixexpr` / `libnixstore` をプロセス内に組み込んでおり、nixpkgs の読み込み・評価状態・daemon 接続を Card 間および複数回の実行をまたいで再利用します。これにより、2 回目以降の準備段階は 9 Card 合計で約 20 ms に収まります（[開発資料の計測](development.md)）。
- キャンセルや `buildTimeoutSeconds` が発生した際は、評価器のスレッドだけを安全に停止します。

### ビルド結果の保持

- すべてのプロジェクトで 1 つの `/nix/store` を共有し、プロジェクトごとの GC root でビルド結果を保持します（Card ごとに直近 2 世代、30 日間未使用のものは自動回収）。
- プロジェクトの「ビルドキャッシュを削除」はそのプロジェクトの GC root を外すだけです。共有 store 自体のガベージコレクションは設定の「共有 store を掃除」からのみ実行されます。`keep-outputs = true` を設定しているため、コンパイラや nixpkgs のソースは掃除後も保持され、Card を編集するたびに再ダウンロードすることはありません。

## 4. Card SDK と worker

### 実行段階

```text
入力待ち → SDK 準備 → Nix 評価 → build → preRun → run → postRun → 出力検証 → 保存 → 完了
```

- 各段階の開始・終了時刻と結果（完了・キャッシュ利用・再利用・失敗・停止）を `InvocationSummary.stages` に記録し、リアルタイム通知と履歴の両方で返します。UI はこの記録だけから段階表示や配線のハイライトを描画し、最終状態からの推測は行いません。
- `preRun` は実行前の準備、`postRun` は成果物の回収や後始末を担います（`run` が失敗した後も `TSUGUMI_FAILED=1` として実行されます）。キャンセルや期限切れの後は `postRun` を含め後続の処理を開始しません。

### 言語 SDK

- **C++**：生成ヘッダー `card.hh` により、ポート名がそのまま型付きのメンバになります。
  ```cpp
  auto run(Card::Input& input) -> Card::Output {
    Card::Output out;
    out.next.emplace().value = input.number.value / 2;
    return out;
  }
  ```
  ビルドされた共有ライブラリ `lib/main.so` を worker がメモリに読み込み、直接呼び出します（呼び出しごとのプロセス起動コストはゼロです）。
- **Python**：`main(ctx)` と、任意の初期化フック `on_run_start(ctx)` を実装します。
- **bash**：入力はポートごとの `JSON_P`（JSON ファイルのパス）/ `FILE_P` / `DIR_P` で受け取り、出力は `$TSUGUMI_OUTPUT` に書き出した JSON をポートごとに型検証します。
- ポート契約に依存しない機能（可視化・ログ・実行コンテキスト）は共通ランタイム `libtsugumi_card.so` に集約し、Card ごとにビルドするのは型定義と薄いアダプタだけに抑えています。

### 常駐 worker

- `reuseWorker`（デフォルト `true`）の worker は実行をまたいで常駐し、static 変数などの状態も保持します（各実行の最初の呼び出し前に `onRunStart` / `on_run_start` が呼ばれます）。
- ビルド条件の変更、失敗・キャンセル、10 分間の未使用、メモリ 2 GiB 超過、または常駐数 32 超過で自動的に終了します。
- `reuseWorker = false` の Card に対しては、次回使う新しい worker を裏で先行起動（サンドボックス起動・`.so` 読み込み・Python の import まで完了）させて待機させます。

### 外部へのアクセス（Experimental）

- Card はデフォルトでネットワークを持たず、Mac 側のファイルシステムも見えません（[security.md](security.md)）。
- Card 設定の Experimental セクションで、Card ごとに「ネットワークへのアクセスを許可する」（`network`）および「ホストのフォルダへのアクセスを許可する」（`hostFolders`。worker からは `/host/<name>` として見え、デフォルトは読み取り専用）を個別に指定できます。

## 5. 文書と保存

- プロジェクトは通常のフォルダです。`tsugumi.json`、`definitions/<Card>/`（`_card.json`・`default.nix`・`src/`・`_outputs.fbs`）、および配置情報の `layout.json` から構成され、そのまま Git でバージョン管理できます。
- JSON はすべて型付きスキーマで厳密に検証（UTF-8・重複キー・非有限数・未知キーの拒否）してから扱います。core は受け取った文書テキストを再エンコードしないため、64 bit 整数の表記などもそのまま保たれます。
- **保存トランザクション**：メモリ上で仮適用して検証 → ジャーナル（全ファイル内容と receipt）を `fsync` → 対象ファイルを原子的に置換 → コミット。万一途中でクラッシュしても、次回起動時にジャーナルから再適用されるため、「全件成功」か「全件未適用」のどちらかにしかなりません（障害注入テスト済み）。
- **競合検出**：保存は編集開始時のリビジョンを基準に行われ、他のウインドウや Agent が先に保存していた場合は競合として検出し、無断で上書きしません。また、同一 `op_id` の再送は 1 回だけ適用されます（通信断で応答を失っても `OperationGet` で結果を照合できます）。
- **実行中の保存**：Workflow の実行中は、計算定義（execution fingerprint）を変える保存を `busy` で拒否し、配置変更などのみを安全に受け付けます。

## 6. 実行履歴と成果物

- 実行・呼び出し・段階・入出力値・ログ・可視化データ・受領記録をすべて SQLite に記録します。正本は VM の実行履歴専用ディスク（`vm/history.img`）に置かれるため、ホストとの共有フォルダを経由せず高速に書き込め、「すべての実行環境を初期化」しても失われません。
- `recordIO` と `retainArtifact` によって、何を記録・保持するかを Card ごとに選択できます。保持された成果物は改ざんやパス逸脱を検出します。
- `saveRunHistory`（デフォルト `true`）が有効なら、完了した実行履歴をプロジェクト内の `.tsugumi/runs.sqlite` にも自動で書き出します。入出力値は FlatBuffers のバイナリ列のまま、スキーマ（bfbs）をハッシュで引ける形で保存するため、後から Card の型定義を変更しても過去の実行値を正確に解釈できます。
- プロジェクトをコピーしたり別の Mac へ移したりしても、開いたときに `run_id` で照合して履歴を自動的に取り込みます。
- 「実行履歴を削除」を実行すると、VM 内・`.tsugumi/runs.sqlite`・実行ごとのファイル領域の 3 か所すべてから削除されます。

## 7. 可視化

- Card のコードから `tsugumi::visualization::text(...)` などの API を呼ぶことで、text・scalar・折れ線・表・画像・3D 点群・Canvas module の出力表示を行えます。
- 折れ線グラフは Dawn（WebGPU 実装、Metal バックエンド）で高速に描画され、Live Stream デモのように実行中もリアルタイムに更新されます。
- Python Card の matplotlib は、SDK の backend（`tsugumi.mpl`）を通じてそのまま Card の出力表示に描かれます。ウィンドウ向けに書いたコードがそのまま動く、X 転送のような仕組みです。`plt.show()` で図を表示し（開いたままの図も呼び出しの終わりに表示）、`plt.pause()` のループや `FuncAnimation` は実行中にライブで更新されます。matplotlib を使う Card には日本語フォント（IPAex）が入り、日本語のラベルもそのまま描けます。
- Plotly の `fig.show()` と Altair の `chart.show()` は、Card の出力表示に対話できるグラフとして出ます（拡大表示でズーム・ホバー・凡例の操作）。描画には同梱の plotly.js / Vega を使い、ネットワークには出ません（Card のグラフは外部からデータを読めません）。Plots デモを参照してください。
- 出力表示は Canvas 上の独立した枠として自由に配置・拡大でき、そのレイアウトは `layout.json` に保存されます。

## 8. UI（ワークベンチ）

- 左：階層・Card 定義・表示中の Card / 中央：Canvas / 下：状態バーとログ・タイムライン / 右：Agent チャット。
- Canvas：Card の追加・移動（グリッド吸着）・複製・配線・範囲選択・ズーム・整列、グループ、Markdown の付箋、出力表示の枠。
- Card 設定ウインドウ：Nix 定義・ソース（構文強調・自動整形・エラー箇所の表示）・入出力・実行履歴・成果物。作業コピー上で編集し、「適用」で反映。
- プロジェクト専用の VM 内ターミナル（そのプロジェクトの名前空間と専用 uid で bash を起動）。
- 日本語 / English の表示切り替え。

画面仕様の詳細は [ui-view.md](ui-view.md) を参照してください。

## 9. Agent

- `agent/` として分離された独立パッケージです。Anthropic Messages API および OpenAI Responses API に対応し、ストリーミングで応答します。OpenAI は API キーのほか、利用者の ChatGPT プラン（Sign in with ChatGPT、オープンソースのツール向けの手順）でも使えます（[セキュリティ](security.md#chatgpt-プランsign-in-with-chatgpt)）。
- Agent が使う tool 定義は `tsugumi.bfbs`（FlatBuffers スキーマの doc comment 付き）から自動生成され、tool 引数と FlatBuffers メッセージの相互変換も同一のスキーマで行います。RPC 契約を更新すれば Agent の tool も自動的に追従します。
- 変更を伴う tool 呼び出しには Agent が一意な `op_id` を付与し、UI とまったく同じリビジョン検査・競合検出・確定通知の経路を通ります。
- **Draft から Card を生成**：Draft の概要・処理内容・ポートと前後の配線を Agent に渡し、型・Nix 定義・ソースコードを 1 トランザクションで保存させたうえで、ビルドと実行による検証まで行わせます。途中の失敗やキャンセルでは中途半端な変更を保存せず、計画は残ります。
- Skill（プロジェクト内の `skills/` や設定で追加したフォルダ）を `$` で呼び出せます。会話の一覧・全文検索・要約圧縮に対応しています。
- **手分け（delegate）**：互いに依存しない作業（線でつながっていない Draft など）は、Agent が helper の Agent に分けて同時に進められます。helper はそれぞれ自分の会話と同じツールを持ち、受け持った Card の定義だけを保存できます（`tsugumi.json`・配置・他の Card は変えられません）。helper 同士の保存と実行がぶつかったとき（実行中の `busy`、`revision_conflict`）は、Agent の側で実行の終わりを待ち、最新のリビジョンから送り直します。helper の報告がそろうと Agent がそれぞれの Card を確かめて報告します。helper の会話は元の会話の隣（`<会話>.helpers/`）に残ります。
- Mac のフォルダをチャットに添付すると、Agent が `AttachedFolderRead` ツールで読み取れます（そのプロジェクトだけに読み取り専用で共有されます。読み取った内容はモデルに送信されます）。
- **ツグミ（チャットとガイド）**：プロジェクトのチャットはツグミです。鳥はチャットの左上（チャットを閉じているときはチャットのボタンの上）で待ち、画面のものを説明するときは `guide_show` でそこへ飛んでいき（▶ 実行、Card、デモのカードなど）、吹き出しで一言話して、タップできる返事（「次へ」「もっと詳しく」など）、押すと動くボタン（デモを開く・実行・停止・Card 設定を開く・API の設定・新規プロジェクトなど）、自由に返事できる入力欄を出します。タップした返事はチャットにユーザーの発言として届きます。吹き出しを閉じると元の場所へ戻ります。動きを減らす設定ではその場に現れます。⌥⌘G でチャット（Home では会話パネル）を開閉します。
  - Home ではアプリ全体で 1 つのガイドの会話（`Agent.Role.guide`、規則は `agent/Shared/guide-prompt.md`）が右下から同じように話します。ガイドは読み取りの tool と `reference_read` と `guide_show` だけを持ち、保存・実行・停止はしません。会話は Mac では `~/Library/Application Support/Tsugumi/agent/guide/`、Web ではデータフォルダの `agent/guide/` に残ります。チャットのツグミが飛び回るのはデモのときだけです。組み込みの Skill `$demo`（`agent/Shared/demo-skill.md`）をデモの案内が始めるか、チャットで `$demo` と書くと始まり、最後のあいさつ（「$demo でいつでもお呼びください…」）で終わります。それ以外はふつうのチャットです。案内の中では Card in Card の中（`card:<外側>/<内側>`、Canvas がその中を開きます）や、ツグミが開く Card 設定のウインドウ（`open`、`param:<名前>`）にも飛んでいきます。プロジェクトを読み込む間は、読み込み中のカードの鳥に重なるように飛び込んで大きさを合わせ、読み込みが終わるとそこから出てきます。
  - 初回起動では Anthropic か OpenAI を選んでどちらか一方の API キーを入力し、接続を確かめてから始めます（「スキップして始める」も選べます）。キーは設定の「API」と同じ経路で Keychain に保存し、すでに使えるキー（または ChatGPT のプラン）があれば何も聞きません。キーのない間は鳥も会話も出さず、キーを登録すると現れ、削除すると消えます。
  - 初めて現れたときは「何に興味がありますか？」と尋ねます（入力するか、候補をタップ）。答えに合うデモを説明とタグから選び、デモ一覧を開いてそのカードへ飛び、理由を一言添えて「このデモを開く」を出します。デモを開くと、そのコピーのチャットのツグミが案内を続けます。案内中は実行の開始・終了（成功・失敗・停止）と接続の切断・再接続を実際の状態のままチャットに伝え、答えている間に起きたことは答え終わってから伝えます。案内の状態と勧誘への回答は保存され、再起動しても繰り返しません。

## 10. デモ

| デモ | 見どころ |
| --- | --- |
| `report`（データからレポートへ） | `seed` による再現性、意図的な失敗と下流の `blocked`、保存済み入力による単体再実行、入れ子の Workflow、ディレクトリ成果物 |
| `collatz`（C++ / Python） | 2 言語の独立実装による結果の一致検証、フィードバックループと `maxRuns`、条件分岐による `skipped`、整数オーバーフローの検出 |
| `live-stream` | 100 ms 間隔のランダムウォークを Dawn（WebGPU / Metal）の折れ線で逐次描画、即時のキャンセル応答 |
| `plots` | ウィンドウ向けの matplotlib のコード（`plt.show()`・`FuncAnimation`）がそのまま Card の表示に描かれる。静止図とライブのアニメーション、Plotly と Altair の対話できるグラフ |
| `casa` | Nix で固定した Chandra の観測（取得は最初の 1 回だけ）、エネルギー帯の分割と RGB 画像、matplotlib のエネルギースペクトル（帯域と輝線）、Card 単位の再実行 |
| `gw150914`・`protein-6lu7`・`exoplanet-transit`・`seismic-trench` | 公開データの取得（出典・大きさ・SHA-256 の表示）と解析・図示 |
| `*-draft` | 計画だけのプロジェクトから Agent の「Card を生成」で実装する |
| `dynamics-draft` | 互いに依存しない 6 つの Draft を、Agent が helper に手分けして同時に実装させる |

各デモの Card の構成と動きは[同梱デモのしくみ](demos.md)を参照してください。デモはホームの「デモを見る」から「コピーを開く」を選ぶことで、`~/Documents/Tsugumi/<demo>` に編集可能なコピーを作ってすぐ試せます。
