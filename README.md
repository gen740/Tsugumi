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
| `tests/python/test_demos.py`（9） | 3 デモの完成条件（上表）、異常終了後の interrupted と再起動、Nix 準備中の状態取得・取消。`TSUGUMI_TEST_CONTAINER=1` で実コンテナ環境に対して実行 |
| `macOS/Tests`（`swift test`） | UI 側 JSON モデル、Agent × ローカル model fixture × 実 tsugumid（読取→設定変更→実行→説明、取消、切断後の再試行で重複実行なし） |
| `tests/python/measure.py` | 評価指標の計測（下記） |

## 計測

（`tests/python/measure.py` の結果。下の「計測結果」節を参照）

## 実装状況

「実装済み」は実装と自動テストで確認したもの、「未検証」は実装したが自動では確認していないもの、「未実装」は今回入れていないものです。
