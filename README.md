# Tsugumi

Tsugumi は、Card をポートで接続したグラフを、Nix で固定した環境で実行・記録する macOS アプリです。
条件分岐（branch）・合流（merge）・有限の feedback loop・入れ子のグラフを扱います。
実行はプロジェクトごとの Apple `container`（軽量 Linux VM）の中の `tsugumid` が行い、
macOS 側の SwiftUI アプリと Agent は同じ FlatBuffers RPC 契約を使う独立したクライアントです。

```text
コンテナ（Apple container, プロジェクトごと）      macOS
 tsugumid (C++ core) ── WebSocket /rpc ──  SwiftUI UI（+ Dawn 描画）
   ├ 文書・journal・receipt                    └ Agent（モデル接続・会話・tool）
   ├ SQLite 履歴・ログ・成果物                ── 同じ RPC ──┘
   ├ スケジューラー・Nix レーン
   └ Card worker（bubblewrap で隔離）
```

## 固定しているバージョン

| 対象 | 版 |
| --- | --- |
| 対応環境 | Apple silicon Mac, macOS 26 以降（検証: macOS 27.2, M3 Max） |
| Apple container | 0.6.0（`macOS/Container/versions.env`、起動管理が版を検査） |
| 環境イメージ | `tsugumi-env:1.0.0`（`macOS/Container/image.nix` から Nix で生成） |
| イメージ生成用ビルダー | `docker.io/nixos/nix:2.31.2` |
| nixpkgs | `1c89083e451e891152365b799595cf7790a43325`（`flake.lock`、Card の既定 `tsugumi:default` も同じ版） |
| FlatBuffers | 25.12.19（C++/Python は nixpkgs、Swift は SwiftPM で同版を固定） |
| hosq（C++ JSON） | `CLI20-dev/hosq@38c3c8e`（flake input） |
| Dawn | `v20261023.194543`（`ui/renderer/CMakeLists.txt`、Metal backend） |

## ディレクトリ

```text
CMakeLists.txt, flake.nix/flake.lock  C++ ビルド（CMake/Ninja）と依存固定（Nix）
protocol/          tsugumi.fbs（外部 RPC）/ worker.fbs / data.fbs / tsugumi_enums.fbs
core/              tsugumid（include/tsugumi/core 公開 API, src 実装）
sdk/               Card ABI（main.so）, C++ SDK, Python SDK と埋め込み shim, Nix SDK, tsugumi-worker
macOS/             SwiftPM: Sources/App（SwiftUI）, Sources/RPC, Sources/Launcher, Sources/Renderer*,
                   Container/（イメージ定義・ビルド・版）, Tests/
ui/renderer/       Dawn 描画層（C API, dylib）
agent/             Agent（SwiftPM パッケージ: provider, 会話, BFBS 由来の tool, JSON⇄FlatBuffers）
tests/cpp, tests/python, tests/fixtures   契約・デモ・計測・ローカル model provider fixture
demos/             UI の Demos 一覧（index.json）と 3 つのデモプロジェクト
```

生成 binding（C++/Python/Swift）と BFBS は `flatc` で各ビルドの出力先に生成し、Git には入れていません。

## ビルドと起動

前提: Apple container 0.6.0（`container system start` 済み）、Nix（flakes 有効）、Xcode または Command Line Tools。

```bash
nix develop
```

```bash
cmake -S . -B build -G Ninja && ninja -C build
```

```bash
cd tests/python && python -m pytest -q test_contract.py
```

環境イメージ（コンテナ内で Nix がビルドするので Linux ホストやリモートビルダーは不要）:

```bash
macOS/Container/build-image.sh
```

ビルダーは `TSUGUMI_BUILDER_CPUS`（既定 8）, `TSUGUMI_BUILDER_MEMORY`（既定 16G）, `TSUGUMI_BUILDER_VOLUME`（Nix キャッシュ volume、既定 `tsugumi-builder-nix`）で調整できます。`TSUGUMI_IMAGE_OUT=dist/tsugumi-env.tar` を付けると、アプリ同梱用の OCI アーカイブも残します。

Dawn 描画層（初回は Dawn と依存の取得・ビルドで数分）:

```bash
ui/renderer/build.sh
```

アプリ本体（`dist/Tsugumi.app`。描画 dylib・デモ・初回用イメージを同梱、ローカル用 ad-hoc 署名）:

```bash
macOS/build-app.sh
```

```bash
open dist/Tsugumi.app
```

開発中は `cd macOS && swift run Tsugumi` でも起動できます（デモはソースツリーから読みます）。

## 使い方（UI からの操作）

1. Home の **Demos** で「Open a Copy」を押すと、同梱原本を `~/Documents/Tsugumi/<demo>` にコピーして開きます（原本は変更しません）。以後ターミナルは不要です。
2. 上部バーに環境の状態（準備中・起動中・準備完了・停止・失敗・再接続）が表示されます。初回はイメージ読込とストア volume 作成が「Preparing」として表示されます。失敗時は原因と復旧手順、環境ログの末尾を表示します。
3. キャンバス: Card のドラッグ移動（複数選択は Shift）、出力ポートから入力ポートへドラッグして接続、Delete で削除、背景ドラッグ/2 本指スクロールでパン、ピンチ/⌘スクロールでズーム、subgraph をダブルクリックで入れ子へ移動（パンくずで戻る）。左のパレットから Card を追加。
4. 右のインスペクターで名前・params・maxRuns・timeout（Inherit / 値 / No limit=null）・各フラグ（Inherit / On / Off）を編集、Source タブで定義のソースを編集します。編集は未確定のまま保持され、⌘Z/⇧⌘Z で Undo/Redo、⌘S（Save）で 1 つの `ProjectSaveRequest` として確定します。保存済みの変更を戻すのも新しい保存です。競合時は編集を保持して理由を表示します。
5. **Run**（全体実行）, **Run Selection**（選択 Card から下流 / 選択 Card のみ＝最後に保存した入力）, **Cancel**。実行中の Card は色と状態、選ばれた分岐はログ（`branch -> a`）で分かります。
6. 下のパネル: Runs & Logs（履歴、stdout/stderr/journal、成果物のプレビューと書出し）、Diagnostics、Agent。
7. 配置・付箋・表示パネル位置は layout.json に、カメラと選択は ViewSave で view ごとに保存され、開き直すと復元されます。

### デモと確認内容

| デモ | 操作 | 期待結果 |
| --- | --- | --- |
| データからレポートへ（`report`） | Run。`settings` の seed / heading を変更して Save→Run。`aggregate` の `fail_on_purpose` を On にして Run、Off に戻して Aggregate を「選択 Card のみ」で再実行。Archive をダブルクリックで入れ子へ。成果物をプレビュー・書出し | 同じ seed で同じ集計（表示が一致）、heading がレポート見出しに反映。失敗時は aggregate の stderr に理由、render は blocked。単体再実行は保存入力で成功。`report`（Directory）と入れ子の `archive` が成果物として残り、環境を作り直しても読める |
| コラッツ C++／Python（`collatz`） | Run。`initial` の value と各言語の `check` の maxRuns（反復上限）を編集 | 27 は 111 変換で 1、初期値を含む 112 値を両言語で表示し `compare` が「C++ and Python agree: 112 values, 111 steps to 1」。maxRuns を下げると execution_limit、3n+1 が uint64 を超える値は「integer overflow」で失敗。value=2 では triple が skipped |
| Live Stream（`live-stream`） | Run。途中で Cancel。interval_ms / seed / samples、plot の window を編集 | 100ms 間隔の乱数歩行が Dawn の折れ線で更新される（直近 60 点）。Cancel は即時応答で停止し最後のグラフが残る。再実行で系列は初期化。並列数 1/2 とも 200 標本が欠落なく順番通り |

## 主な設計上の決定

- **JSON は typed schema**（hosq）で扱います。文書は `json::GenericDom` で全体検証（UTF-8・重複キー・非有限数）してから struct に decode し、未知キーは拒否します。hosq はメンバー単位の省略付き encode を持たないため、**文書ファイルの正本（canonical）はクライアントから受け取った検証済みテキストそのもの**です。core は文書を再エンコードしないので、元の整数表記（64 bit 含む）がそのまま保持・転送されます。core 内部の JSON（journal・view・job 等）は全フィールド必須の struct として encode します。
- **保存**: 変更は文書モデルに仮適用して検証 → journal（全ファイル内容と receipt を含む）を fsync → ファイルを原子的置換 → commit 記録。途中で落ちても起動時に journal から再適用し、全件成功か全件未適用のどちらかになります（障害注入テストあり）。
- **実行中の保存**: 計算定義（execution fingerprint）を変える保存は run 中 `busy` で拒否し、配置などの変更は受け付けます（ドラッグを止めないため）。
- **pkgsPath**: `tsugumi:default`（同梱 pin）または `https://github.com/NixOS/nixpkgs/archive/<rev>.tar.gz#sha256-…`。Card の Nix 評価は restricted eval・IFD 禁止・allowed-uris 限定です。
- **組み込み Card のポート**: trigger `out` / input（`_outputs.fbs` の各メッセージを同名 params から構築、File は選んだプロジェクトファイルを run にコピー）/ fork `in`→params `branches` / branch `value`,`condition`(value:bool)→`a`,`b` / merge 宣言 inputs→`out`（入力ポート名付き union `tsugumi.merge.Merged`）/ logger `in` / subgraph は `boundary`。
- **隔離**: ホストと共有するのはプロジェクトフォルダ・0700 の control フォルダ（server.ready と import grant）・永続ストア volume だけです。RPC は 127.0.0.1 にのみ公開し、token は control ファイル（0600）にだけ置いて URL・ログに出しません。Card はコンテナ内で bubblewrap（全 namespace 分離・ネットワークなし・`/nix/store` 読取専用・自分の作業領域だけ書込可・環境変数は空から構成）で実行し、`sandbox: false` でも同じ unprivileged uid のまま、ホスト実行や追加公開はしません。bubblewrap が使えない環境では `sandbox: true` の Card を拒否します。
- **永続化**: コンテナの `/nix` はイメージ（下層）＋ volume（上層）の overlay で、Card 用ツールチェーンは volume に残ります（コンテナ再作成で失われない）。停止（`stop`）とデータ削除（store volume の削除）は別操作で、文書・履歴は常にプロジェクトフォルダに残ります。
- **二重起動防止**: ホストではプロジェクト実体（device+inode）ごとの flock で起動・停止を直列化し、別名パスでも同じ環境に接続します。core は `.tsugumi/tsugumid.lock` に非待機 flock を取得し、さらに同じファイルへの 2 本目の flock が拒否されること（ロックが実際に効く保存先か）を確認してから文書を開きます。
- **Agent**: `agent/` の独立パッケージ。tool 定義は `tsugumi.bfbs`（FBS の doc comment 付き）から生成し、tool 引数⇄FlatBuffers も同じ BFBS で変換します（64 bit は 10 進文字列）。op_id は tool 呼び出しごとに Agent が付け、会話・tool 証跡と一緒にホスト側へ永続化します。応答を失った変更は OperationGet で照合し、盲目的に再送しません。モデルの API キーは Keychain に保存し core/worker には渡しません。取消はモデル通信と未送信 tool を止め、受理済み run は止めません。

## テスト

| テスト | 内容 |
| --- | --- |
| `tests/python/test_contract.py`（24） | §6.7 の往復例、同時保存で片方だけ成功、同一 op_id の同時再送・再起動後再送、障害注入 3 点、重複キー/未知キー/不正 ID、uint64 最大値と表記保持、明示 false/0、Hello 規則、未知 union・不正長で切断、二重起動拒否、ソースの hash 競合・UTF-8 境界 patch・分割読取、外部変更と Reload、View の CAS、日本語ログの byte 境界・tail・範囲外、ページングと cursor の条件違い、SDK 準備と失効、CacheClear、import grant、遅い peer の切断と他 peer の継続 |
| `tests/python/test_demos.py`（9） | 3 デモの完成条件（上表）、異常終了後の interrupted と再起動、Nix 準備中の状態取得・取消。`TSUGUMI_TEST_CONTAINER=1` で実コンテナ環境に対して実行（最終確認: 9 passed） |
| アプリ smoke（`TSUGUMI_SMOKE_DEMO=<id> TSUGUMI_SMOKE_OUT=<json> open dist/Tsugumi.app`） | アプリ自身のコード経路で デモのコピー→環境起動→接続→配置編集の保存→実行→Display 受信→Undo を新しい保存として確定、を 3 デモとも確認 |
| `macOS/Tests`（`swift test`） | UI 側 JSON モデル、Agent × ローカル model fixture × 実 tsugumid（読取→設定変更→実行→説明、取消、切断後の再試行で重複実行なし） |
| `tests/python/measure.py` | 評価指標の計測（下記） |

## 計測（2026-11-02）

条件: MacBook Pro M3 Max（36 GB）, macOS 27.2, Apple container 0.6.0, 環境 2 vCPU / 4 GiB / store volume 20 GiB,
デモは各 1 回（Live Stream の継続利用は 10 回）。アプリの smoke モード（`TSUGUMI_SMOKE_DEMO`）と
`tests/python` のスクリプトで、実コンテナ環境に対して計測しました。

| 指標 | 実測 | 目標との比較 |
| --- | --- | --- |
| 基本イメージ | OCI 圧縮 93.5 MB / 展開後 `/nix/store` 272 MB | 大きい依存: glibc 47 MB, ICU 42 MB と Boost 18 MB（Nix 本体の依存）, util-linux 29 MB（overlay の mount だけに使用。静的 mount に置換可能）, bash 9.5 MB, OpenSSL 9 MB, SQLite 6 MB, tsugumid 一式 6 MB |
| Nix 依存取得後の store（プロジェクトごとの volume 上層） | 約 816 MB | gcc 280 MB（C++ Card）, 固定 nixpkgs ソース 200 MB（Card の Nix 評価）, Python 146 MB, perl 59 MB・binutils 38 MB（stdenv）。10 回実行での増加 0 |
| 履歴・成果物 | Live Stream 1 回で SQLite 約 5 MB（WAL 込み）、10 回で +8.8 MB。Report の成果物 29 KB | ログと I/O 記録量に比例 |
| 起動（キャッシュ済みイメージ, 新規コピーのデモを開く→操作可能） | 1.5〜3.1 s（コンテナ起動・store volume 作成・Hello・Open を含む） | 目標 10 s 以内を達成 |
| 初回準備（新しい store へのツールチェーン取得＋ビルド＋実行） | Collatz 22.5 s, Report 13.4 s, Live Stream 36.7 s（うち標本間隔の待機 20 s） | 回線速度に依存（cache.nixos.org） |
| 応答（Live Stream 実行中の別接続からの RunStatus、100 回） | p50 2.1 ms / p99 10.0 ms | RPC は 100 ms 以内。SwiftUI の入力応答は画面での計測を行っていない（下の「未検証」） |
| 取消 | RunCancel 応答 8.3 ms、Live Stream の worker 停止→cancelled まで 0.12 s。Nix 準備中の取消 0.11 s（別計測、実コンテナ） | 目標（100 ms / 2 s）を達成 |
| 継続利用（Live Stream 10 回、各 24.2〜24.8 s） | 残存 worker/sandbox プロセス 0→0、store 増加 0、履歴 +8.9 MB（WAL 込み）、tsugumid RSS 12.2→15.0 MB | 当初は終了済み sandbox helper の zombie が 1 回あたり 2 個残っていた（tsugumid が PID 1 で reap しなかった）。tini を PID 1 にして解消し、再計測で 0 を確認 |

## 実装状況

「実装済み」は実装と自動テストまたは実機の自動操作で確認したもの、「未検証」は実装したが確認できていないもの、「未実装」は今回入れていないものです。

| 領域 | 状況 |
| --- | --- |
| FBS 契約（全要求・成功型・Failure・通知・worker.fbs）、flatc による C++/Python/Swift binding 生成 | 実装済み |
| tsugumid: Hello/Open/Close/Inspect/Save/Reload/Validate/Import/CacheClear/OperationGet/FileList/Stat/Read/Format(json)/SDKPrepare/Status/RunPlan/Run/Cancel/Status/History/Invocations/Record/ValueRead/ArtifactList/Read/LogRange/ViewSetVisible/ViewSave | 実装済み（契約テスト）。Format は json のみ、他言語は `unsupported` |
| 保存 transaction・journal・receipt・障害復旧・二重起動防止 | 実装済み |
| スケジューラー（all/any、有界 queue、背圧、maxRuns、skipped/blocked、並列 1〜64、取消、期限、再起動時 interrupted） | 実装済み |
| 型導出（_outputs.fbs、merge union、File/Directory、構造的互換性） | 実装済み |
| Nix レーン（restricted eval・IFD 禁止・キャッシュ・GC root・取消・期限）、C++/Python/shell Card | 実装済み |
| bubblewrap による Card 隔離、環境変数の非継承、`sandbox:false` の扱い | 実装済み（実コンテナ） |
| Apple container 環境（最小イメージ、永続 store overlay、停止とデータ削除の分離、ローカル限定公開） | 実装済み |
| SwiftUI アプリ（Home/Demos、キャンバス、インスペクター、ソース編集、Undo/Redo、保存競合、実行・取消、履歴・ログ・成果物、Agent パネル、環境状態表示、複製） | 実装済み・アプリの実コード経路を smoke モードで 3 デモとも確認。**画面表示とマウス操作の目視確認は未検証**（この作業環境では画面収録・アクセシビリティ操作の権限がなく、スクリーンショットを取得できなかったため） |
| Dawn 描画層（Metal、サイズ変更・高 DPI・フレーム単位の集約・資源解放） | 実装・ビルド済み。描画結果の目視確認は未検証 |
| Agent（provider 設定・Keychain・モデル一覧、streaming、BFBS 由来 tool、引数検証、実 RPC、取消、再試行、会話と証跡の永続化・再開、文脈圧縮） | 実装済み（fixture 経由の読取→変更→実行→説明、取消、切断再試行を自動テスト）。文脈圧縮は自動テストなし。実 provider（有料 API）での動作は未検証 |
| 外部 Logger DB | `sqlite:<name>` のみ（`.tsugumi/logger/`）。他の DB は `unsupported` |
| N-Queens・200 Card の大規模デモ、Web UI、3D・WASM 表示、Draft 変換 | 未実装（仕様上の次段階） |

## 既知の問題

- 開発中、Apple container 0.6.0 で一部の VM が `stop`/`rm` に応答しなくなる状態が発生しました（`container ls` では running のまま、`container system stop` でも残る）。原因の一つは、tsugumid 自身が PID 1 だったため sandbox helper（bubblewrap の中間プロセス）の zombie が回収されず VM の終了処理が進まなかったことと考えられ、イメージでは tini を PID 1 にしました。すでに固まった VM は CLI からは止められないため、macOS の再起動などで container の実行状態を初期化してください。固まった VM がキャッシュ volume を掴んでいる間は、`TSUGUMI_BUILDER_VOLUME` で別の volume を指定するとイメージをビルドできます。
- 環境はプロジェクトごとに独立した Nix store volume を持つため、ツールチェーン（約 0.8 GB）はプロジェクトごとに取得されます（隔離を優先した設計）。
