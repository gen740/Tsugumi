<p align="center">
  <img src="docs/assets/tsugumi-hero.svg" alt="Tsugumi" width="100%">
</p>

# Tsugumi

Tsugumi は、Card をポートで接続したグラフを、Nix で固定した環境で実行・記録する macOS アプリです。
条件分岐（branch）・合流（merge）・有限の feedback loop・入れ子のグラフを扱います。
実行はアプリ全体で 1 台の Linux VM（Virtualization.framework を直接使用）の中で、開いているプロジェクトごとの `tsugumid` が行います。
macOS 側の SwiftUI アプリと Agent は同じ FlatBuffers RPC 契約を使う独立したクライアントです。

```text
共通 VM（アプリに 1 台、Virtualization.framework）              macOS
 tsugumi-vmd（vsock: 制御 1024 / RPC 1025）
 nix-daemon ── 共通 /nix/store（store ディスク）
 project A: tsugumid（uid・mount/PID 名前空間・cgroup を分離）── vsock ─ 127.0.0.1 中継 ── SwiftUI UI（+ Dawn 描画）
   ├ 文書・journal・receipt（/project = virtiofs）                                     └ Agent（モデル接続・会話・tool）
   ├ SQLite 履歴・ログ・成果物                                         ── 同じ RPC ──┘
   ├ スケジューラー・Nix レーン
   └ Card worker（bubblewrap で隔離）
 project B: tsugumid（同上。A の停止は B に影響しない）
```

## 固定しているバージョン

| 対象 | 版 |
| --- | --- |
| 対応環境 | Apple silicon Mac, macOS 26 以降（検証: macOS 27.2, M3 Max） |
| 共通 VM | カーネル 6.12.28（`macOS/Container/kernel.nix`、SELinux なし）+ 読み取り専用 rootfs（erofs、`macOS/Container/vm.nix`） |
| VM イメージ生成用ビルダー | `docker.io/nixos/nix:2.31.2` を Apple container 0.6.0 で実行（開発時のビルドだけ。アプリの実行には使わない） |
| nixpkgs | `1c89083e451e891152365b799595cf7790a43325`（`flake.lock`、Card の既定 `tsugumi:default` も同じ版） |
| FlatBuffers | 25.12.19（C++/Python は nixpkgs、Swift は SwiftPM で同版を固定） |
| hosq（C++ JSON） | `CLI20-dev/hosq@38c3c8e`（flake input） |
| Dawn | `v20261023.194543`（`ui/renderer/CMakeLists.txt`、Metal backend） |

## ディレクトリ

```text
CMakeLists.txt, flake.nix/flake.lock  C++ ビルド（CMake/Ninja）と依存固定（Nix）
protocol/          tsugumi.fbs（外部 RPC）/ worker.fbs / data.fbs / tsugumi_enums.fbs
core/              tsugumid（include/tsugumi/core 公開 API, src 実装）
sdk/               Card ABI, 共通 runtime（libtsugumi_card）, C++ codegen（card.hh）, Python SDK と埋め込み shim, Nix SDK, tsugumi-worker
macOS/             SwiftPM: Sources/App（SwiftUI）, Sources/RPC, Sources/Launcher, Sources/Renderer*,
                   Container/（イメージ定義・ビルド・版）, Tests/
ui/renderer/       Dawn 描画層（C API, dylib）
agent/             Agent（SwiftPM パッケージ: provider, 会話, BFBS 由来の tool, JSON⇄FlatBuffers）
tests/cpp, tests/python, tests/fixtures   契約・デモ・計測・ローカル model provider fixture
demos/             UI の Demos 一覧（index.json）と 3 つのデモプロジェクト
docs/assets/       アイコンとキービジュアルの SVG
```

生成 binding（C++/Python/Swift）と BFBS は `flatc` で各ビルドの出力先に生成し、Git には入れていません。

## ビルドと起動

前提: Nix（flakes 有効）、Xcode または Command Line Tools。VM イメージを作るときだけ Apple container 0.6.0（`container system start` 済み）を使います。

```bash
nix develop
```

```bash
cmake -S . -B build -G Ninja && ninja -C build
```

```bash
cd tests/python && python -m pytest -q test_contract.py
```

共通 VM のイメージ（カーネルと rootfs を `dist/tsugumi-vm/` に。コンテナ内で Nix がビルドするので Linux ホストやリモートビルダーは不要）:

```bash
macOS/Container/build-image.sh --vm
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

VM を使うには virtualization の entitlement 付きの署名が必要なので、開発中も `macOS/build-app.sh` で作った `dist/Tsugumi.app` を使います（`swift run` の実行ファイルは署名がなく VM を起動できません）。テストが VM を操作する `tsugumi-vmctl` は `macOS/tools/build-vmctl.sh` で作ります。

アプリアイコン `macOS/Resources/AppIcon.icns` は `docs/assets/tsugumi-icon.svg` から生成してコミットしています。SVG を変えたら `swift macOS/Resources/tools/make_icon.swift` で作り直します。

## 使い方（UI からの操作）

画面構成と操作の仕様は [ui-view.md](ui-view.md) です。UI の文言は日本語が基準で、アプリ設定 > 一般 > 言語で English に切り替えられます（再起動後に反映。英語表は `macOS/Resources/tools/en_strings.py`）。

1. **初期設定**: この Mac で仮想マシンを使えないか、アプリに VM イメージがないときだけ、メインウインドウに準備画面が出ます。それ以外の準備（VM イメージの展開、Nix store の保存領域の作成と初期化、VM の起動）は最初にプロジェクトを開いたときに行い、鳥の loading に段階を表示します。「コンソールで詳細を見る」（ウインドウ > 実行環境のコンソール、⌥⌘K）で VM の起動ログを追えます。
2. **ホーム / デモ**: ホームで新規作成・既存フォルダ・最近のプロジェクトを開きます。デモは「コピーを開く」で `~/Documents/Tsugumi/<demo>` に編集可能なコピーを作って開きます（原本は変更しません）。起動・読込中は進捗を画面に重ね、失敗時は理由・復旧手順・環境ログと再試行を出します。
3. **ワークベンチ**（左: 階層・Card 定義・表示中の Card、中央: Canvas、下: 状態バーとログ/タイムライン、右: チャット）。子 Workflow はダブルクリックで同じタブの階層を切り替え、階層一覧の右クリックで新しいタブ／別ウインドウに開けます（同じ階層のタブは再利用）。実行対象は常に文書の Workflow 全体です。
4. **Canvas**: Card の追加（定義のダブルクリック／ドラッグ）・移動（グリッド吸着）・複製（⌘D）・削除・折りたたみ、出力ポートから入力ポートへのドラッグで接続（線をクリックして「切断」）、Shift ドラッグで範囲選択、パン・ピンチ/⌘スクロールのズーム、全体表示・整列。入力 Card は値を Card 上で直接編集できます。グループ（名前・所属・公開出力、見た目だけ）、Markdown の付箋（色・位置・サイズ）、出力表示の枠（移動・サイズ変更・拡大ウインドウ・枠の削除）を置けます。上中央に実行・停止・経過時間・進捗・結果・問題一覧、右上に Draft があるときの「Card を生成」。
5. **Card の右クリック**: 設定、実行履歴、成果物、ログ、チャットへの参照追加、ここから実行、複製、折りたたみ、出力表示、グループ化、切断、削除。
6. **Card 設定**（別ウインドウ）: 一般・Nix 定義・ソース（ファイルの作成・名前変更・削除）・入出力・実行履歴・成果物・追加コンテキストを、Card の種類に応じて表示します。編集は画面内の作業コピーで、「未適用の変更」を明示し「適用」で Workflow へ反映・保存します（「閉じる」も適用してから閉じます）。共通定義の変更はその定義を使う全 Card に反映されます。Draft は専用画面で名前・処理の説明・仮の入出力（仮の出力は `_outputs.fbs` になり先に配線できる）を編集します。
7. **保存と同期**: Canvas の編集は未確定のまま保持し、⌘Z/⇧⌘Z、⌘S またはツールバーの「保存」で 1 つの `ProjectSaveRequest` として確定します。状態バーは実行状態と保存・同期状態を分けて表示します。他のウインドウや Agent が先に保存した場合、未保存の変更は**編集を始めた revision を基準**に保存されるので競合として検出され、上書きしません。変更は保持され、「変更をコピー」と（確認付きの）「最新を読み込む」を選べます。実行中は Card の変更とプロジェクト切替を制限し、閲覧は続けられます。
8. **チャット**: 会話の一覧・全文検索、Card / 可視化の参照追加、`$` で Skill（プロジェクトの `skills/` と設定で追加したフォルダ）、モデル選択、送信／停止（Workflow 実行の停止とは別）、再試行、会話の圧縮（全文履歴は残る）。別ウインドウにも出せます。
9. **設定**: Workflow 設定（一般・データベース接続先・ビルドキャッシュ）と、アプリ設定（一般: 言語・並列度・送信キー / 外観: テーマ・グリッド・線の動き / エディター: インデント幅・ガイド / ショートカットの変更 / Skills / API / 外部接続: 接続先のコピー）。
10. 配置・付箋・グループ・出力枠は layout.json に、カメラと選択は ViewSave で view ごとに保存され、開き直すと復元されます。

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
- **隔離**: VM と共有するのは開いているプロジェクトのフォルダと、その 0700 の control フォルダ（server.ready と import grant）だけです（virtiofs、プロジェクトを閉じると共有から外す）。VM 内では tsugumi-vmd がプロジェクトごとに専用の uid・mount/PID/IPC/UTS 名前空間・cgroup で tsugumid を起動し、そのプロジェクトの `/project` と `/control` 以外（他のプロジェクト、共有の全体、store ディスク）は見えません。あるプロジェクトの停止はその cgroup だけを終わらせます。RPC は VM の vsock から Mac の 127.0.0.1 にだけ中継し、token は control ファイルにだけ置いて URL・ログに出しません。virtiofs は全ファイルを見る側の所有として見せるため、`sandbox: false` の Card からは control フォルダと `.tsugumi` を空の tmpfs で隠します（ファイルの mode に頼らない）。Card は bubblewrap（全 namespace 分離・ネットワークなし・`/nix/store` 読取専用・自分の作業領域だけ書込可・環境変数は空から構成、nobody に対応づけた uid）で実行します。bubblewrap が使えない環境では `sandbox: true` の Card を拒否します。
- **永続化と実行環境の置き場所**: 実行環境のデータは `~/Library/Application Support/Tsugumi/` にあります。`vm/` に VM イメージの展開先（`image/`、アプリ同梱から APFS clone）、全プロジェクト共通の Nix store の sparse なディスクイメージ `store.img`（既定で最大 64 GiB、使った分だけ）、コンソールログ（`console.log`・前回分の `console.prev.log`・アプリ側の記録 `launcher.log`）を置き、`environments/<id>/control/` に各プロジェクトの control フォルダを置きます。VM は store ディスクを `/persist` にし、`/nix` は rootfs の store（下層）＋ `/persist`（上層）の overlay で、nix-daemon 1 つを全プロジェクトが使います。各プロジェクトのビルド結果の GC root は `/nix/var/nix/gcroots/tsugumi/projects/<id>` にあり、閉じているプロジェクトのビルドも CacheClear の GC で消えません。設定の「すべての実行環境を初期化」は VM を止めて `vm/` と `environments/` を削除し（以前の版が Apple container に作った環境もあれば削除）、プロジェクトの文書・履歴・成果物やホストの `/nix/store` には触れません。
- **二重起動防止**: VM は `vm/vm.lock` の flock を持つアプリ 1 つだけが起動します（2 つ目のアプリは「別の Tsugumi が実行環境を使っています」で止まる）。core は `.tsugumi/tsugumid.lock` に非待機 flock を取得し、さらに同じファイルへの 2 本目の flock が拒否されること（ロックが実際に効く保存先か）を確認してから文書を開きます。
- **Card の実行段階**: 入力待ち → SDK 準備（入出力・params の契約生成）→ Nix 評価（`nix-instantiate`）→ build（`nix-store --realise`）→ 実行（`preRun → run（.so）→ script → postRun`）→ 出力検証 → 保存 → 完了。core は呼び出しごとに通った段階を開始・終了時刻と結果（完了・キャッシュ利用・この Run で準備済みの再利用・失敗・停止）つきで `InvocationSummary.stages` に記録し、通知と履歴の両方で返します。worker の再利用（`worker_reused`）、実際に値を送った出力ポート（`outputs_sent`、branch は選んだ側だけ）、入力が揃った時刻（`ready_at`）も持ちます。UI はこの記録だけから段階表示・配線の強調を作り、最終状態から推測しません。期限は呼び出し全体で 1 つ、各段階の開始前に取消・期限を確認します。preRun / postRun は worker を再利用しても invocation ごとに、その作業ディレクトリ・入出力の中で動きます。preRun の環境変数と `cd` は後段に引き継ぎ、postRun は失敗後も後始末として動きます（`TSUGUMI_FAILED=1`、前段の失敗は消さない）。停止・期限切れの後は postRun を含めて何も始めません（旧名 preScript / postScript も受け付けます）。
- **Card SDK**: `default.nix` で契約から SDK を作り、言語ごとの package に渡します。SDK は `languages` に挙げた言語の分だけ生成・build し、visualization・context・ログなど契約に依存しない部分は環境ごとに 1 度だけ build する共通の `libtsugumi_card.so` にしています（Card ごとに作るのは型と薄い adapter だけ）。

  ```nix
  { pkgs, inputs, params }:
  let
    sdk = inputs.tsugumi.sdk { languages = [ "cpp" ]; };   # inputs / outputs / params を明示して渡すこともできる
    package = inputs.tsugumi.cppCard { inherit sdk; src = ./src; };
  in
  {
    inherit package;
    run = "${package}/lib/main.so";   # worker が読み込む shared object
    preRun = "mkdir -p scratch";      # 任意。postRun も同様
  }
  ```

  ```cpp
  #include <card.hh>

  auto run(Card::Input& input) -> Card::Output {
    Card::Output out;
    out.next.emplace().value = input.number.value / 2;   // ポート名がそのまま member
    tsugumi::visualization::text("halved");
    return out;
  }
  ```

  `Card::Input` は入力ポートごとの member（任意入力は `std::optional`、merge は入力ポート名付き union）、`Card::Output` は出力ポートごとの `std::optional`（設定したものだけ送る）、`Card::params()` が params です（数値・bool は未設定を表せる `flatbuffers::Optional` なので `.value()` / `.value_or(x)` で読む）。File / Directory は `.value` のパスで、出力先は `tsugumi::context().output_directory`。失敗は `throw tsugumi::Failure(kind, message)`、停止の確認は `tsugumi::cancelled()`。生成ヘッダー（`_inputs_generated.h` など）を Card が include する必要はありません。`sdk` を使わない旧形式（`#include <tsugumi/sdk/card.hh>` と `TSUGUMI_CARD`）も当面は動きます。
- **shell の入力**: ポート `P` ごとに `JSON_P`（JSON ファイルのパス、大きさの制限なし）、`BIN_P`（FlatBuffer のパス）、`INPUT_P`（4 KiB 以下の値だけ: 単一の `value` ならその値、他は JSON 文字列）、File/Directory は `FILE_P`/`DIR_P`。出力は `$TSUGUMI_OUTPUT` の JSON をポートごとに型検証します。スクリプトはファイル経由で実行するので大きさの制限を受けません。
- **Nix の上流参照**: `default.nix` は `inputs.upstream."<Card path>"`（`inputs.local.<id>` は同じ階層）で、**どの経路で発火しても必ず先に実行される**コード Card の属性集合（`package`・スクリプト・`exports` の関数や値・store context）を参照できます。同じポートへの複数経路は共通部分、`all` は必須入力の和、`any` は全入力の共通部分、ループは最大不動点で求め、分岐の片側だけの Card や下流・自分自身は渡しません（静的診断 `nix_reference_*`）。参照されたものだけが遅延評価され、非選択分岐は build しません。ビルドの鍵はソース・入力型・Nix 用 params・参照する上流定義で決まり、実行時だけの params は含みません。
- **Card in Card**: 子 Workflow は `boundary.inputs/outputs` で外側のポートを内部 Card のポートに対応づけ、外からは通常の Card として接続・実行できます（Run の対象にでき、単体実行では境界入力に保存済みの値を使う）。状態と履歴は完全な Card path で区別します。見た目だけのグループ（layout.json）とは別物です。
- **Draft**: `_meta.json` に概要・処理内容・制約・完了条件・入出力の用途を持ち、型なしの仮ポートで配線できます。実行も成功扱いもせず、Draft に依存する Card は理由付きで blocked、独立した部分はそのまま実行します。「Card を生成」は Agent に Draft・配線・説明を渡し、PutDefinition/PutSource の 1 トランザクションで型・Nix・ソースを保存してから検証・実行させます（途中の失敗や取消では何も保存されず、計画は残ります）。
- **Agent**: `agent/` の独立パッケージ。tool 定義は `tsugumi.bfbs`（FBS の doc comment 付き）から生成し、tool 引数⇄FlatBuffers も同じ BFBS で変換します（64 bit は 10 進文字列）。op_id は tool 呼び出しごとに Agent が付け、会話・tool 証跡と一緒にホスト側へ永続化します。応答を失った変更は OperationGet で照合し、盲目的に再送しません。モデルの API キーは Keychain に保存し core/worker には渡しません。取消はモデル通信と未送信 tool を止め、受理済み run は止めません。

## テスト

| テスト | 内容 |
| --- | --- |
| `tests/python/test_contract.py`（26） | §6.7 の往復例、同時保存で片方だけ成功、同一 op_id の同時再送・再起動後再送、障害注入 3 点、重複キー/未知キー/不正 ID、uint64 最大値と表記保持、明示 false/0、Hello 規則、未知 union・不正長で切断、二重起動拒否、ソースの hash 競合・UTF-8 境界 patch・分割読取、外部変更と Reload、View の CAS、日本語ログの byte 境界・tail・範囲外、ページングと cursor、SDK 準備と失効、CacheClear、import grant、遅い peer、UI 用 layout 項目と Draft、整形（JSON/C++/Python/Nix・構文エラー位置・formatter 不在・保存しない） |
| `tests/python/test_regressions.py`（4） | 報告された不具合の再現: 履歴保存失敗で run が成功にならない、File の fork 転送、CacheClear・再起動後も単体再実行の保存入力（File 実体を含む）が使える、保存入力の不足は明示的に失敗 |
| `tests/python/test_scheduler.py`（6） | any の到着順消費・欠落/重複なし、all の 1 値ずつ消費と optional 出力の非発火、maxRuns、Card in Card の単体実行・伝播・同じ内部 ID の path 区別、保存入力なしの明示的失敗、Start→入力 Card と Draft 依存の blocked |
| `tests/python/test_history.py`（7） | `recordIO × retainArtifact` 全組合せ×CacheClear×再起動、成果物の改ざん検出・パス逸脱、大容量のページング、UTF-8 境界、過去と最新の記録の分離 |
| `tests/python/test_transport.py`（5） | WebSocket の Origin/subprotocol/token 拒否、fragment・ping 割り込み・連結、Unix の 1 byte ずつ・連結フレーム、実行元切断後も run 継続・別接続から状態取得 |
| `tests/python/test_cards_container.py`（11） | 実環境（共通 VM。`TSUGUMI_TEST_ENV=container` で以前の Apple container）: C++ SDK（`card.hh` の `run(Card::Input&) -> Card::Output`、merge union・任意出力・File 出力・params・visualization、明示した `run` の .so、再利用 worker でも invocation ごとの preRun/postRun、2 つの SDK が同じ共通 runtime を使うこと）、背圧で進行不能な run の終了、shell 契約（preRun の cd/環境、300 KB 入力、期限の継続、post 失敗）、取消で子孫プロセスまで終了（sandbox on/off）、隔離（プロジェクト・token・ネットワーク）、Nix 上流参照と拒否、段階とキャッシュ、Python の表示 4 種、Python/C++/shell 間の値（64 bit 境界・10 万要素・enum/struct/union・別名前空間・File・破損 buffer・必須出力欠落） |
| `tests/python/test_shared_vm.py`（2） | 共通 VM: 2 プロジェクトが 1 つの Nix store を共有しつつ uid・見えるファイル・プロセス（PID 名前空間）が分かれていること、片方を止めてももう片方の実行が成功し、止めた方も同じ VM で再開できること |
| `tests/python/test_demos.py`（9） | 3 デモの完成条件（Collatz は独立実装の参照値と照合、子 Workflow の単体実行を含む）、異常終了後の interrupted と再起動、Nix 準備中の状態取得・取消。`TSUGUMI_TEST_CONTAINER=1` で実コンテナ環境に対して実行 |
| `tests/cpp`（`tsugumi_core_tests`、15） | 型（catalog・構造的互換・merge union）、型の伝播、Nix 上流参照（直線・分岐+any・all と optional・同一ポート複数経路・feedback loop・入れ子・到達不能な循環・診断）、run plan（全体・単体・子 Workflow）、履歴の状態選択と障害注入、送信キュー（順序・seq・遅い peer の隔離） |
| アプリ smoke（`TSUGUMI_SMOKE_DEMO=<id> TSUGUMI_SMOKE_OUT=<json>`、`TSUGUMI_SMOKE_RERUN=1` で 2 回目の実行も） | アプリ自身のコード経路で デモのコピー→環境起動→接続→配置編集の保存→実行→Display→Undo、付箋・グループ・折りたたみ・出力枠の保存、Draft の仮出力、古い基準の下書きが競合になり上書きしないこと |
| UI スクリプト（`TSUGUMI_UI_SCRIPT=<file>`） | 実ウインドウを操作してウインドウ単位で撮影: 長い日本語名と title/sidebar、sidebar 開閉、ウインドウ縮小、Card 設定（ソース・プレビュー）、未保存編集のままカテゴリ移動と外部更新→競合で内容保持、Draft、設定の Skills→一般、エディター、外観プレビュー、Workflow 設定 |
| `macOS/Tests`（`swift test`、25） | JSON モデル、段階とフロー方向の幾何、ファイル階層、params の整数表記、Skill の読込、ショートカット、Agent × ローカル model fixture × 実 tsugumid（読取→変更→実行→説明、取消、再試行、Draft の計画→保存→再読込→実装→検証、生成取消で計画が残る） |
| `tests/python/measure.py` | 評価指標の計測（下記） |

## 計測（2026-11-02）

条件: MacBook Pro M3 Max（36 GB）, macOS 27.2, Apple container 0.6.0, 環境 2 vCPU / 4 GiB / store volume 20 GiB,
デモは各 1 回（Live Stream の継続利用は 10 回）。アプリの smoke モード（`TSUGUMI_SMOKE_DEMO`）と
`tests/python` のスクリプトで、実コンテナ環境に対して計測しました。

| 指標 | 実測 | 目標との比較 |
| --- | --- | --- |
| 基本イメージ | OCI 圧縮 93.5 MB / 展開後 `/nix/store` 272 MB（整形ツール astyle・ruff・nixfmt・shfmt を加えた現行版は圧縮 103 MB。clang-format は LLVM 全体で約 540 MB 増えるため入れていない） | 大きい依存: glibc 47 MB, ICU 42 MB と Boost 18 MB（Nix 本体の依存）, util-linux 29 MB（overlay の mount だけに使用。静的 mount に置換可能）, bash 9.5 MB, OpenSSL 9 MB, SQLite 6 MB, tsugumid 一式 6 MB |
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
| tsugumid: Hello/Open/Close/Inspect/Save/Reload/Validate/Import/CacheClear/OperationGet/FileList/Stat/Read/Format/SDKPrepare/Status/RunPlan/Run/Cancel/Status/History/Invocations/Record/ValueRead/ArtifactList/Read/LogRange/ViewSetVisible/ViewSave | 実装済み（契約テスト）。Format は JSON・C++（clang-format、イメージでは astyle）・Python（ruff）・Nix（nixfmt）・shell（shfmt）、FBS は `unsupported` |
| 実行段階の通知・キャッシュ表示、Nix 上流参照、Card in Card の実行、Draft の仮ポートと blocked 理由、表示（text・scalar・折れ線・表・画像・3D 点群・Canvas module） | 実装済み（C++/Python/コンテナのテスト） |
| 保存 transaction・journal・receipt・障害復旧・二重起動防止 | 実装済み |
| スケジューラー（all/any、有界 queue、背圧、maxRuns、skipped/blocked、並列 1〜64、取消、期限、再起動時 interrupted） | 実装済み |
| 型導出（_outputs.fbs、merge union、File/Directory、構造的互換性） | 実装済み |
| Nix レーン（restricted eval・IFD 禁止・キャッシュ・GC root・取消・期限）、C++/Python/shell Card | 実装済み |
| bubblewrap による Card 隔離、環境変数の非継承、`sandbox:false` の扱い | 実装済み（実コンテナ） |
| Apple container 環境（最小イメージ、永続 store overlay、停止とデータ削除の分離、ローカル限定公開） | 実装済み |
| SwiftUI アプリ（ui-view.md の構成、split view の sidebar とチャット inspector、上から下へ流れる Canvas〔共通の Card 表現・段階表示・グループ・付箋・出力枠・Card in Card の作成〕、Card 設定〔実 Card と同じプレビュー・既定値と上書きの区別・構文強調/整形/エラー位置/生成 _inputs.fbs・外部更新との競合保持・境界ポート〕、Draft 編集と生成、定義 tree、表示 7 種、設定〔Skills の非同期読込・エディター見本・操作できる外観プレビュー〕） | 実装済み。UI スクリプトで実ウインドウを撮影して確認（上の表）。マウスのクリック・ドラッグは未確認 |
| Dawn 描画層（Metal、サイズ変更・高 DPI・フレーム単位の集約・資源解放） | 実装・ビルド済み。Live Stream の折れ線の目視確認は未実施 |
| Agent（provider 設定・Keychain・モデル一覧、streaming、BFBS 由来 tool、引数検証、実 RPC、取消、再試行、会話と証跡の永続化・再開、文脈圧縮） | 実装済み（fixture 経由の読取→変更→実行→説明、取消、切断再試行を自動テスト）。文脈圧縮は自動テストなし。実 provider（有料 API）での動作は未検証 |
| 外部 Logger DB | `sqlite:<name>` のみ（`.tsugumi/logger/`）。他の DB は `unsupported` |
| N-Queens・200 Card の大規模デモ、Web UI、WASM 表示 | 未実装（仕様上の次段階） |

## 既知の問題

- **VM が終了できず残る問題（原因と対策）**: Apple container 0.6.0 の推奨カーネル（6.12.28）では、マウント名前空間が片付けられるとき（Nix のビルド sandbox・bubblewrap・コンテナの終了）に SELinux の inode 解放処理 `selinux_inode_free_security` が NULL を参照して kernel oops になり、そのプロセスが終わらず VM が終了処理に進めませんでした（`container logs --boot` に `Internal error: Oops` が出ます）。このため `stop`/`rm` が固まり、`tsugumi-image-builder-*` やテスト環境が残っていました。Tsugumi は SELinux を使わない（Card の隔離は bubblewrap の名前空間）ので、同じ設定から SELinux だけを外した専用カーネル（`macOS/Container/kernel.nix`、`macOS/Container/build-image.sh --kernel` で `dist/tsugumi-kernel`）を作り、アプリ・テスト・イメージのビルダーが自分の container にだけ `--kernel` で渡します（システムの既定カーネルは変えません）。この変更後、container テスト 20 件と、起動→Collatz 実行→停止→削除の 8 回の繰り返し（`tests/python/stress_shutdown.py`）で oops 0 件・残存 0 でした。変更前に固まった VM は CLI からは止められないことがあるため、macOS の再起動で解放してください。
- CacheClear の Nix GC が環境自身の実行時パスまで回収し、次の起動で entrypoint が壊れる問題がありました。イメージの実行時 closure を GC root にし、起動時に上層の whiteout を取り除くよう修正済みです（既存の volume も自動で回復します）。
- UI の確認は `TSUGUMI_UI_SCRIPT` による実ウインドウ操作とウインドウ単位のスクリーンショットです。この環境ではアクセシビリティ権限がなく、**マウスでのクリック・ドラッグ（Canvas/プレビューのドラッグ、ポートの接続）は自動確認していません**。
- 環境はプロジェクトごとに独立した Nix store（ディスクイメージ）を持つため、ツールチェーン（約 0.8 GB）はプロジェクトごとに取得されます（隔離を優先した設計）。以前の版が作った named volume（`tsugumi-<id>-store`）は、その環境のデータ削除または「すべての実行環境を初期化」で削除されます。
- 環境イメージは Apple container の管理領域に置かれるため、Tsugumi のサポートフォルダを削除しただけでは即座には消えません（次の起動で同梱イメージに置き換え、孤立した container を削除します）。完全に消すには設定の「すべての実行環境を初期化」を使ってください。
