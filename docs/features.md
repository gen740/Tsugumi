# Tsugumi の主要機能

Tsugumi は、Card をポートで接続したグラフ（Workflow）を、Nix で固定した環境で実行し、その結果をすべて記録する macOS アプリです。
この文書では、アプリの中心になる機能を「何ができるか」と「どう実現しているか」の両面からまとめます。
ビルド手順は [INSTALL.md](../INSTALL.md)、テスト一覧・計測値は [Development.md](Development.md)、隔離と秘密情報の扱いは [security.md](security.md)、他ツールとの違いは [novelty.md](novelty.md) を参照してください。

## 1. 全体構成

```text
macOS（ホスト）                                   共通 Linux VM（アプリに 1 台）
 SwiftUI UI ─┐                                     tsugumi-vmd（VM の管理役）
 Agent ──────┼─ FlatBuffers RPC ─ 127.0.0.1 ─ vsock ─ project A: tsugumid ─ worker プール
 外部クライアント┘                                   project B: tsugumid ─ worker プール
                                                   nix-daemon ─ 共通 /nix/store
```

- **tsugumid（core）**: プロジェクトごとに 1 プロセス。文書の管理・保存、RPC、スケジューラー、Nix 評価、worker 管理、実行履歴をすべて持ちます。UI がなくても RPC だけで保存・実行・履歴取得ができます。
- **UI と Agent は対等なクライアント**: どちらも `protocol/tsugumi.fbs` の同じ RPC 契約で core に話します。Agent は UI を操作するのではなく、core に直接 RPC を送ります。Agent を無効にしても通常の操作はすべて成立します。
- **共通 VM**: Virtualization.framework で 1 台の Linux VM を起動し、その中でプロジェクトごとに分離した tsugumid を動かします（詳細は [security.md](security.md)）。

## 2. Card と Workflow

### Card の種類

| 種類 | 役割 |
| --- | --- |
| コード Card | `default.nix` で環境を、`src/` で処理を定義する。C++ / Python / bash |
| trigger | Workflow の起点（`out`） |
| input | `_outputs.fbs` の型に沿った値を Canvas 上で直接編集して流す。File は選んだファイルを run にコピー |
| fork / branch / merge | 分配・条件分岐（`condition` で `a` / `b` の片側だけ発火）・合流（入力ポート名付き union） |
| loop | 有限の feedback loop（ループ上の Card の `maxRuns` で反復上限） |
| logger | 値を外部 DB に記録（現在は `sqlite:<name>` のみ、`.tsugumi/logger/`） |
| subgraph（Card in Card） | 子 Workflow を 1 つの Card として扱う。`boundary` で外側のポートを内部に対応づける |
| Draft | 型のない仮ポートで先に配線できる「未実装の Card」。Agent が実装を生成する |

### 型付きのポート

- ポートの型は FlatBuffers スキーマ（`_outputs.fbs`）で宣言し、入力型は上流から導出されます。接続は構造的互換性で検査し、merge は自動で union 型になります。
- 値は FlatBuffers のバイト列のまま Card 間を流れます。C++ / Python / bash 間で 64 bit 整数・enum・struct・union・File / Directory が欠落なく受け渡せることをテストで確認しています。

### 実行の意味論（スケジューラー）

- 入力の待ち方は `all`（全入力が揃ったら 1 値ずつ消費）と `any`（到着順に消費）。
- 有界 queue と背圧、並列度 1〜64、取消、期限（呼び出し全体で 1 つ）、`maxRuns`。
- 非選択分岐の下流は `skipped`、Draft や失敗に依存する Card は理由付きで `blocked`。独立した部分はそのまま進みます。
- 再起動時に途中だった実行は `interrupted` として記録されます。
- 「ここから実行」「選択 Card のみ」の単体再実行では、前回保存した入力（File の実体を含む）を使います。

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

- `pkgs` は既定で同梱の固定 nixpkgs（`tsugumi:default`）。任意の nixpkgs を `https://github.com/NixOS/nixpkgs/archive/<rev>.tar.gz#sha256-…` で固定できます。
- 評価は restricted eval・IFD（import from derivation）禁止・取得先 URL を nixpkgs アーカイブに限定。
- Tsugumi 独自のビルドキャッシュはありません。評価した derivation の出力が store にあればビルドしない、という Nix 本来の仕組みだけで判定します。

### 上流定義の参照（`inputs.upstream`）

- `default.nix` から、**どの経路で発火しても必ず先に実行される**上流 Card の定義（`run`・`preRun`・`exports` の関数や値）を参照できます。
- 「必ず先に実行される」の判定は、`all` / `any`・複数経路・ループ（最大不動点）・入れ子を考慮した静的解析です。分岐の片側だけにある Card や下流・自分自身は参照できず、静的診断（`nix_reference_*`）になります。
- 参照されたものだけが遅延評価され、選ばれなかった分岐はビルドしません。

### 評価器の組み込み

- tsugumid は libnixexpr / libnixstore をプロセス内に組み込み、nixpkgs の読み込み・評価状態・daemon 接続を Card と実行をまたいで使い回します。2 回目以降の準備段階は 9 Card 合計で約 20 ms です（[開発資料の計測](Development.md)）。
- 取消・`buildTimeoutSeconds` は評価器のスレッドだけを止めます。

### ビルド結果の保持

- 全プロジェクトで 1 つの `/nix/store` を共有し、プロジェクトごとの GC root で結果を保持します（Card ごとに直近 2 つ、30 日未使用で回収）。
- 「ビルドキャッシュを削除」はそのプロジェクトの root を外すだけ。共有 store の実際の掃除はアプリ設定の「共有 store を掃除」でだけ行います。`keep-outputs = true` なので、コンパイラや nixpkgs のソースは掃除後も残り、Card を編集しても再ダウンロードしません。

## 4. Card SDK と worker

### 実行段階

```text
入力待ち → SDK 準備 → Nix 評価 → build → preRun → run → postRun → 出力検証 → 保存 → 完了
```

- 各段階の開始・終了時刻と結果（完了・キャッシュ利用・再利用・失敗・停止）を `InvocationSummary.stages` に記録し、通知と履歴の両方で返します。UI はこの記録だけから段階表示・配線の強調を描き、最終状態から推測しません。
- `preRun` は準備、`postRun` は成果物と後始末（失敗後も `TSUGUMI_FAILED=1` で動く）。停止・期限切れの後は何も始めません。

### 言語 SDK

- **C++**: 生成ヘッダー `card.hh` により、ポート名がそのまま member になります。
  ```cpp
  auto run(Card::Input& input) -> Card::Output {
    Card::Output out;
    out.next.emplace().value = input.number.value / 2;
    return out;
  }
  ```
  共有ライブラリ `lib/main.so` を worker が読み込み、メモリ経由で呼び出します（プロセス起動のコストなし）。
- **Python**: `main(ctx)` と任意の `on_run_start(ctx)`。
- **bash**: 入力はポートごとの `JSON_P`（JSON ファイル）/ `FILE_P` / `DIR_P`。出力は `$TSUGUMI_OUTPUT` の JSON をポートごとに型検証します。
- 契約に依存しない部分（可視化・ログ・context）は共通の `libtsugumi_card.so` に入れ、Card ごとに作るのは型と薄い adapter だけです。

### 常駐 worker

- `reuseWorker`（既定 true）の worker は実行をまたいで常駐し、static な状態も保ちます（各実行の最初に `onRunStart`）。
- ビルド条件の変更・失敗・取消・10 分未使用・2 GiB 超過・常駐数 32 超過で終了。
- `reuseWorker = false` の Card には、次に使う worker を先に起動しておきます（sandbox 起動・`.so` 読み込み・Python の import まで）。

### 外部へのアクセス（Experimental）

- Card は既定でネットワークを持たず、Mac のファイルも見えません（[security.md](security.md)）。
- Card 設定の Experimental で、Card ごとに「ネットワークへのアクセスを許可する」（`network`）と「ホストのフォルダへのアクセスを許可する」（`hostFolders`。worker から `/host/<name>` に見え、既定は読み取り専用）を指定できます。

## 5. 文書と保存

- プロジェクトはフォルダです。`tsugumi.json`、`definitions/<Card>/`（`_card.json`・`default.nix`・`src/`・`_outputs.fbs`）、配置の `layout.json` から成り、Git で管理できます。
- JSON は typed schema で全体検証（UTF-8・重複キー・非有限数・未知キー拒否）してから扱います。core は文書を再エンコードしないので、64 bit 整数の表記もそのまま残ります。
- **保存トランザクション**: 仮適用して検証 → journal（全ファイル内容と receipt）を fsync → ファイルを原子的に置換 → commit。途中で落ちても起動時に journal から再適用し、全件成功か全件未適用のどちらかになります（障害注入テストあり）。
- **競合検出**: 保存は編集を始めた revision を基準に行います。他のウインドウや Agent が先に保存していたら競合として返り、上書きしません。同じ `op_id` の再送は 1 回だけ適用されます（応答を失っても `OperationGet` で照合）。
- **実行中の保存**: 計算定義（execution fingerprint）を変える保存は `busy` で拒否し、配置などは受け付けます。

## 6. 実行履歴と成果物

- 実行・呼び出し・状態・値・ログ・表示・受領記録を SQLite に記録します。置き場所は VM の実行履歴ディスク（`vm/history.img`。共有フォルダを通らないので速く、「すべての実行環境を初期化」でも残る）。
- `recordIO` と `retainArtifact` で何を残すかを Card ごとに選べます。成果物は改ざん・パス逸脱を検出します。
- `saveRunHistory`（既定 true）なら、終わった実行をプロジェクト内の `.tsugumi/runs.sqlite` にも書き出します。値は FlatBuffers のバイト列のまま、型（bfbs）を hash で引ける形で保存するため、実行ごとに型が変わっても後から解釈できます。
- プロジェクトをコピーしたり別の Mac に移したりしても、開いたときに `run_id` で照合して履歴を取り込みます。
- 「実行履歴を削除」は VM・`.tsugumi/runs.sqlite`・実行ごとのファイルの 3 か所から消します。

## 7. 可視化

- Card から `tsugumi::visualization::text(...)` などで表示を出せます。text・scalar・折れ線・表・画像・3D 点群・Canvas module の 7 種。
- 折れ線は Dawn（WebGPU 実装、Metal backend）で描画し、Live Stream デモのように実行中に更新されます。
- 出力表示は Canvas 上の枠として配置・拡大でき、配置は `layout.json` に保存されます。

## 8. UI（ワークベンチ）

- 左: 階層・Card 定義・表示中の Card / 中央: Canvas（上から下へ流れる）/ 下: 状態バーとログ・タイムライン / 右: チャット。
- Canvas: 追加・移動（グリッド吸着）・複製・接続・範囲選択・ズーム・整列、グループ（見た目だけ）、Markdown の付箋、出力表示の枠。
- Card 設定ウインドウ: Nix 定義・ソース（構文強調・整形・エラー位置）・入出力・実行履歴・成果物。作業コピーで編集し「適用」で反映。
- 編集は未確定のまま保持し、⌘Z / ⇧⌘Z、⌘S で 1 つの保存要求として確定します。
- プロジェクトの VM 内ターミナル（そのプロジェクトの名前空間と uid で開く）。
- 日本語 / English の切り替え。

画面仕様の詳細は [ui-view.md](../ui-view.md) です。

## 9. Agent

- `agent/` の独立パッケージ。Anthropic Messages API / OpenAI Responses API に対応し、ストリーミングで応答します。
- tool 定義は `tsugumi.bfbs`（FBS の doc comment 付き）から自動生成し、tool 引数⇄FlatBuffers も同じスキーマで変換します。RPC 契約を変えれば Agent の tool も追従します。
- 変更を伴う tool には Agent が `op_id` を付け、UI と同じ revision・競合検査・確定通知を通ります。
- **Draft から Card を生成**: Draft の概要・処理内容・制約・完了条件・配線を Agent に渡し、型・Nix・ソースを 1 トランザクションで保存してから検証・実行させます。途中の失敗や取消では何も保存されず、計画は残ります。
- Skill（プロジェクトの `skills/` と設定で追加したフォルダ）を `$` で呼び出せます。会話の一覧・全文検索・圧縮（全文履歴は残る）。
- Mac のフォルダをチャットに添付すると、Agent が `AttachedFolderRead` で読めます（読み取り専用、そのプロジェクトだけに共有。読んだ内容はモデルに送られます）。

## 10. デモ

| デモ | 見どころ |
| --- | --- |
| `report`（データからレポートへ） | seed による再現性、意図的な失敗と blocked、単体再実行、入れ子の Workflow、Directory 成果物 |
| `collatz`（C++ / Python） | 2 言語の独立実装の結果一致、feedback loop と `maxRuns`、branch による skipped、overflow の検出 |
| `live-stream` | 100 ms 間隔の値を Dawn の折れ線で逐次表示、即時の Cancel |

デモは「コピーを開く」で `~/Documents/Tsugumi/<demo>` に編集可能なコピーを作って開きます。
