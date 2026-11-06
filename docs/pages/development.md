# 開発資料

Tsugumi の実行基盤・設計上の意思決定・テストスイート・パフォーマンス計測・実装状況をまとめた開発者向け資料です。
アプリの導入とビルド手順は [INSTALL.md](install.md)、依存ライブラリの固定バージョンは [Lock.md](lock.md)、利用者向けの全体像は [README](../index.md) を参照してください。
本文中のファイルパスとコマンドは、リポジトリのルートを基準としています。
現在の機能は[実装状況](#実装状況)、確認方法は[確認範囲](#確認範囲)、以前の構成で直した不具合は[解決済みの記録](#解決済みの記録)に分けて記載します。計測は各見出しの日付・構成での記録です。

## 実行基盤

ポートの型スキーマで配線の整合性を検査し、Nix によって Card ごとの実行環境をハッシュ単位で固定します。
実行履歴と成果物を永続化し、各 Card の入出力値も設定に応じて記録します。

macOS アプリの計算は Mac 上の隔離された Linux VM 内で実行され、Card からの外部ネットワークアクセスは既定で遮断されます。Web UI は Mac 上の `tsugumid` を直接使い、Card を Seatbelt で隔離します（[Web UI の実行環境と範囲](webui.md)）。
一方、外部 LLM を利用する Agent には会話内容やツールが読み取った情報（ソースコード・ログ・入出力値など）が送信されるため、計算環境の隔離と外部モデルへのデータ送信範囲は区別して管理する必要があります。詳細は[セキュリティと隔離](security.md#モデルに送られるもの)にあります。

グラフの実行エンジンは、条件分岐（branch）・合流（merge）・有限のフィードバックループ・入れ子の子 Workflow（Card in Card）をサポートします。
macOS アプリでは、アプリ全体で 1 台の共通 Linux VM（`Virtualization.framework` を直接使用）の中で、開いているプロジェクトごとに独立した `tsugumid` プロセスが実行を担います。
メインウインドウは 1 つのプロジェクトを表示し、複数のウインドウで同時に開けます（[UI の仕様](ui-view.md#複数のプロジェクト)）。Web UI はブラウザのタブごとです。
macOS 側の SwiftUI アプリと Agent は、どちらも同じ FlatBuffers RPC 契約を介して `tsugumid` と通信する対等なクライアントです。

```text
共通 VM（アプリに 1 台、Virtualization.framework）              macOS
 tsugumi-vmd（vsock: 制御 1024 / RPC 1025）
 nix-daemon ── 共通 /nix/store（store ディスク）
 project A: tsugumid（uid・mount/PID 名前空間・cgroup を分離）── vsock ─ 127.0.0.1 中継 ── SwiftUI UI（+ Dawn 描画）
   ├ 文書・journal・receipt（/project = virtiofs）                                     └ Agent（モデル接続・会話・tool）
   ├ SQLite 履歴・ログ・成果物                                         ── 同じ RPC ──┘
   ├ スケジューラー・Nix 評価器（libnixexpr をプロセス内に組み込み）
   └ worker プール（実行をまたいで常駐・待機 worker、bubblewrap で隔離）
 project B: tsugumid（同上。A の停止は B に影響しない）
```

## ディレクトリ

```text
CMakeLists.txt, flake.nix/flake.lock  C++ ビルド（CMake/Ninja）と依存固定（Nix）
protocol/          tsugumi.fbs（外部 RPC）/ worker.fbs / data.fbs / tsugumi_enums.fbs
core/              tsugumid（include/tsugumi/core 公開 API, src 実装）
sdk/               Card ABI, 共通 runtime（libtsugumi_card）, C++ codegen（card.hh）, Python SDK と埋め込み shim, R・Julia SDK（別プロセス）, Nix SDK, tsugumi-worker
macOS/             SwiftPM: Sources/App（SwiftUI）, Sources/RPC, Sources/Launcher, Sources/Renderer*,
                   Container/（イメージ定義・ビルド・版）, Tests/
ui/renderer/       Dawn 描画層（C API, dylib）
ui/modules/        Card のグラフ（Plotly・Vega-Lite）の表示部品
agent/             Agent（SwiftPM パッケージ: provider, 会話, BFBS 由来の tool, JSON⇄FlatBuffers）
web/               ブラウザ UI とホストサーバー（Agent の契約は agent/Shared と共通）
plugins/tsugumi/   Claude Code の plugin（.claude-plugin/marketplace.json から入れる）
nix/               core.nix: tsugumid・tsugumi-worker・Card SDK のパッケージ定義
tests/cpp, tests/python, tests/fixtures   契約・デモ・計測・ローカル model provider fixture
demos/             UI の Demos 一覧（index.json）、基本・科学・Plots（matplotlib・Plotly・Altair）のデモと科学デモの Draft 版
docs/              文書（一覧は docs/README.md）。docs/assets/ にアイコン・キービジュアル・実画面のスクリーンショット、
                   docs/site/ に GitHub Pages の生成器と動く図（生成物は docs/index.html と docs/pages/）
```

生成 binding（C++/Python/Swift）と BFBS は `flatc` で各ビルドの出力先に生成し、Git には入れていません。

## 主な設計上の決定

- **JSON は typed schema**（hosq）で扱います。文書は `json::GenericDom` で全体検証（UTF-8・重複キー・非有限数）してから struct に decode し、未知キーは拒否します。hosq はメンバー単位の省略付き encode を持たないため、**文書ファイルの正本（canonical）はクライアントから受け取った検証済みテキストそのもの**です。core は文書を再エンコードしないので、元の整数表記（64 bit 含む）がそのまま保持・転送されます。core 内部の JSON（journal・view・job 等）は全フィールド必須の struct として encode します。
- **保存**: 変更は文書モデルに仮適用して検証 → journal（全ファイル内容と receipt を含む）を fsync → ファイルを原子的置換 → commit 記録。途中で落ちても起動時に journal から再適用し、全件成功か全件未適用のどちらかになります（障害注入テストあり）。
- **実行中の保存**: 計算定義（execution fingerprint）を変える保存は run 中 `busy` で拒否し、配置などの変更は受け付けます（ドラッグを止めないため）。
- **pkgsPath**: `tsugumi:default`（同梱 pin）または `https://github.com/NixOS/nixpkgs/archive/<rev>.tar.gz#sha256-…`。Card の Nix 評価は restricted eval・IFD 禁止・allowed-uris 限定です。
- **組み込み Card のポート**: trigger `out` / input（`_outputs.fbs` の各メッセージを同名 params から構築、File は選んだプロジェクトファイルを run にコピー）/ fork `in`→params `branches` / branch `value`,`condition`(value:bool)→`a`,`b` / merge 宣言 inputs→`out`（入力ポート名付き union `tsugumi.merge.Merged`）/ logger `in` / subgraph は `boundary`。
- **隔離**（全体は[セキュリティと隔離](security.md)）
  - **VM と共有するもの**: 開いているプロジェクトのフォルダと、その 0700 の control フォルダ（server.ready と import grant）、そして Card 定義やチャットで指定された Mac のフォルダ（Card の `hostFolders` は `host-<tag>`、チャットの添付は `chat-<tag>` として同じスロットに。`write: true` 以外は読み取り専用）だけです。virtiofs で共有し、プロジェクトを閉じると共有から外します。
  - **VM の中のプロジェクト**: tsugumi-vmd がプロジェクトごとに専用の uid・mount/PID/IPC/UTS 名前空間・cgroup で tsugumid を起動します。そのプロジェクトの `/project`・`/control`・`/history` と、そのプロジェクトに共有した Mac のフォルダ（`/run/tsugumi/host/`・`/run/tsugumi/chat/`）以外（他のプロジェクト、共有の全体、store ディスク）は見えません。あるプロジェクトの停止はその cgroup だけを終わらせます。
  - **RPC**: VM の vsock から Mac の 127.0.0.1 にだけ中継し、token は control ファイルにだけ置いて URL・ログに出しません。
  - **`sandbox: false` の Card**: virtiofs は全ファイルを見る側の所有として見せるため、control フォルダと `.tsugumi` を空の tmpfs で隠します（ファイルの mode に頼らない）。
  - **Card**: bubblewrap（全 namespace 分離・ネットワークなし・`/nix/store` 読取専用・自分の作業領域だけ書込可・環境変数は空から構成、nobody に対応づけた uid）で実行します。Experimental の `network: true` の Card は network 名前空間を分けず（`--share-net`、名前解決と CA のファイルを読取専用で見せる）、`hostFolders` で指定した Mac のフォルダは worker の `/host/<name>` に bind します。
  - **bubblewrap がないとき**: Linux で使えない環境では `sandbox: true` の Card を拒否します。Mac 上の tsugumid は Seatbelt を使います（[Web UI](webui.md)）。
- **永続化と実行環境の置き場所**
  - **データの場所**: `~/Library/Application Support/Tsugumi/`（リリース版。ソースからビルドした開発版は `Tsugumi-dev/`、`AppChannel.swift`。`TSUGUMI_SUPPORT_DIR` で VM と store の置き場所だけを別にもできる）。
  - **`vm/` の中身**: VM の起動ディスクの展開先（`image/`、アプリ同梱から APFS clone）、VM に読み取り専用で渡す Tsugumi のソース（`source/`、`macOS/Container/vm-source.txt` に挙げたもの。起動ごとにコピー）、全プロジェクト共通の Nix store の sparse なディスクイメージ `store.img`（既定で最大 64 GiB、使った分だけ）、コンソールログ（`console.log`・前回分の `console.prev.log`・アプリ側の記録 `launcher.log`）。`environments/<id>/control/` に各プロジェクトの control フォルダを置きます。
  - **store**: VM は store ディスクを `/persist` にし、`/nix` は rootfs の store（下層）＋ `/persist`（上層）の overlay で、nix-daemon 1 つを全プロジェクトが使います。
  - **Tsugumi 自身のビルド**: tsugumid・tsugumi-vmd・ターミナルの道具・整形ツールは、VM の `/init` が共有されたソースの flake（`packages.<system>.vm-system`）を VM の中の Nix でビルドして起動します。依存は cache.nixos.org から取り、tsugumid はソースからコンパイルします。ソースの内容の hash ごとに結果を `/persist/system/<hash>` に記録して GC root にするので、作り直すのは初回とソースが変わったとき（アプリの更新、開発中の core の変更）だけです。Mac 側には何もダウンロードしません。
  - **起動ディスクの更新**: 変わったとき（アプリの更新）は 1 度だけ store を確かめ（`nix-store --verify --repair`）、前の起動ディスクにあって消えたパスのうち、Card のビルドなどから参照されているものは cache.nixos.org から取り直し、参照のないものはデータベースから外します。
  - **GC root**: 各プロジェクトのビルド結果は `/nix/var/nix/gcroots/tsugumi/projects/<id>/<Card>--<結果>` です（フォルダは vmd が root として作り、親フォルダはプロジェクトから書けないので他のプロジェクトが先取りできない）。結果は使うたびに一時 root（daemon 接続が持つ）→ 恒久 root の順に確保するので、有効性の確認・使用と GC が競合しません。Card ごとに直近 2 つの結果を残し、30 日使われない root と、30 日起動されていないプロジェクトの root フォルダは回収します。
  - **掃除と初期化**: プロジェクト設定の「ビルドキャッシュを削除」はそのプロジェクトの root を外すだけで、共有 store の掃除（root のないものの削除、vmd の `gc`）はアプリ設定の「共有 store を掃除」でだけ行います。`keep-outputs = true` なので、root のある Card のビルドに使ったコンパイラや nixpkgs のソースは掃除しても残ります。設定の「すべての実行環境を初期化」は VM を止めて `vm/` と `environments/` を削除し（以前の版が Apple container に作った環境もあれば削除）、実行履歴のディスク `vm/history.img`、プロジェクトの文書・成果物、ホストの `/nix/store` には触れません。
- **実行履歴の置き場所**
  - **正本**: 履歴の SQLite（実行・呼び出し・状態・値・ログ・表示・受領記録）は VM の実行履歴用ディスク（`vm/history.img`、最大 32 GiB の sparse、`/history-disk` に mount）の `<id>/history.sqlite` にあり、そのプロジェクトの tsugumid にだけ `/history` として見えます（`--history-dir`。指定がなければ `.tsugumi/history.sqlite`）。Card をまたぐたびの確定待ちが共有フォルダを通らなくなります。
  - **ディスクの設定**: store ディスク（`synchronizationMode: .full`、確定ごとに Mac 側で F_FULLFSYNC）と分け、`.fsync` で付けます（アプリや VM が落ちても確定済みの記録は残る。Mac の電源断では直前の書き込みを失うことがある。共有フォルダの同期書き込みの速さから見て、共有フォルダ上に置いていたときと同程度と推定）。store とは別に初期化でき、「すべての実行環境を初期化」でも残ります。
  - **プロジェクト内のコピー**: Workflow 設定の「実行履歴をプロジェクトに保存する」（`tsugumi.json` の `saveRunHistory`、既定 true）がオンなら、終わった実行を `.tsugumi/runs.sqlite` にも追記します（書き込みが 2 秒止んだとき、遅くとも 30 秒後と、終了時。実行の時間には入らない）。
  - **コピーの形式**: 決まった形（`meta` の `format = tsugumi-runs`・`version = 2`）で、値は FlatBuffers のバイト列のまま SHA-256 を添えて `schemas`（hash → bfbs・root type）を指し、実行ごとに型が変わっても読む側がその場で解釈できます。ログは書いた断片と位置のまま（Run 全体のログの順序を保つ）、成果物はプロジェクト内のファイル `.tsugumi/runs/<run>/` を指します。実行中の状態・受領記録・最後の入力は含めません。
  - **持ち運び**: 開くと `run_id` で照らし合わせて VM にない実行を読み込むので、コピーや別の Mac へ移したプロジェクトでも履歴がついて来ます。
  - **削除**: 「実行履歴を削除」（`RunHistoryDelete`、全部か指定日時より前に終わったもの、実行中は不可）は VM・`.tsugumi/runs.sqlite`・実行ごとのファイルの 3 か所から消します。VM 内の履歴は自動では消さず、アプリ設定の「VM 内の実行履歴」で容量を見て、フォルダがもうないプロジェクトの分も消せます（vmd の `histories`・`forget`）。
- **二重起動防止**: VM は `vm/vm.lock` の flock を持つアプリ 1 つだけが起動します（2 つ目のアプリは「別の Tsugumi が実行環境を使っています」で止まる）。core は `.tsugumi/tsugumid.lock` に非待機 flock を取得し、さらに同じファイルへの 2 本目の flock が拒否されること（ロックが実際に効く保存先か）を確認してから文書を開きます。
- **Card の実行段階**: 入力待ち → SDK 準備（入出力・params の契約生成）→ Nix 評価（tsugumid に組み込んだ評価器）→ build（daemon。store に結果があれば何もしない）→ 実行（`preRun → run → postRun`）→ 出力検証 → 保存 → 完了。core は呼び出しごとに通った段階を開始・終了時刻と結果（完了・キャッシュ利用・この Run で準備済みの再利用・失敗・停止）つきで `InvocationSummary.stages` に記録し、通知と履歴の両方で返します。worker の再利用（`worker_reused`）、実際に値を送った出力ポート（`outputs_sent`、branch は選んだ側だけ）、入力が揃った時刻（`ready_at`）も持ちます。UI はこの記録だけから段階表示・配線の強調を作り、最終状態から推測しません。期限は呼び出し全体で 1 つ、各段階の開始前に取消・期限を確認します。preRun / postRun は worker を再利用しても invocation ごとに、その作業ディレクトリの中で動き、Card の入力は受け取りません（preRun は準備、postRun は成果物と後始末: `TSUGUMI_OUTPUT_DIR`・送った出力ポート `TSUGUMI_OUTPUTS`・`TSUGUMI_FAILED`）。preRun の環境変数と `cd` は後段に引き継ぎ、postRun は失敗後も後始末として動きます（`TSUGUMI_FAILED=1`、前段の失敗は消さない）。停止・期限切れの後は postRun を含めて何も始めません。
- **Card SDK**: `default.nix` で契約から SDK を作り、Card 本体を `run` に書きます。derivation（package）なら worker がその `lib/main.so` を読み込んでメモリ経由で呼び、文字列なら bash として実行します（どちらか一方）。SDK は `languages` に挙げた言語の分だけ生成・build し、visualization・context・ログなど契約に依存しない部分は環境ごとに 1 度だけ build する共通の `libtsugumi_card.so` にしています（Card ごとに作るのは型と薄い adapter だけ）。

  ```nix
  { pkgs, inputs, params }:
  let
    sdk = inputs.tsugumi.sdk { languages = [ "cpp" ]; };   # inputs / outputs / params を明示して渡すこともできる
  in
  {
    run = inputs.tsugumi.cppCard { inherit sdk; src = ./src; };   # package
    preRun = "mkdir -p scratch";                                  # 任意。postRun も同様
  }
  ```

  ```nix
  { pkgs, inputs, params }: {
    run = ''
      jq '{out: {value: (.value + 1)}}' "$JSON_IN" > "$TSUGUMI_OUTPUT"
    '';                                                           # bash
  }
  ```

  ```cpp
  #include <card.hh>

  static std::vector<uint64_t> seen;               // worker は実行をまたいで常駐する
  auto onRunStart() -> void { seen.clear(); }      // 任意: 各実行の最初の呼び出しの前

  auto run(Card::Input& input) -> Card::Output {
    Card::Output out;
    out.next.emplace().value = input.number.value / 2;   // ポート名がそのまま member
    tsugumi::visualization::text("halved");
    return out;
  }
  ```

  `Card::Input` は入力ポートごとの member（任意入力は `std::optional`、merge は入力ポート名付き union）、`Card::Output` は出力ポートごとの `std::optional`（設定したものだけ送る）、`Card::params()` が params です（数値・bool は未設定を表せる `flatbuffers::Optional` なので `.value()` / `.value_or(x)` で読む）。File / Directory は `.value` のパスで、出力先は `tsugumi::context().output_directory`。失敗は `throw tsugumi::Failure(kind, message)`、停止の確認は `tsugumi::cancelled()`。Python は `main(ctx)` と任意の `on_run_start(ctx)` です。生成ヘッダー（`_inputs_generated.h` など）を Card が include する必要はありません。
- **worker**: プロジェクトの worker プールが Card ごとの worker を持ちます。`reuseWorker`（既定 true）の worker は実行をまたいで常駐し（static な状態も残る。各実行の最初に `onRunStart` / `on_run_start`）、ビルド・実行条件の変更、失敗・取消・期限切れ、プロジェクトの停止、10 分の未使用、2 GiB を超えるメモリ、常駐数 32 を超えた古いもので終わります。`reuseWorker = false` の Card には、使うたびに次の新しい worker を先に起動しておきます（sandbox・worker の起動・`lib/main.so` の読み込み・Python の import まで。preRun は呼び出しごと）。読み込みは worker 自身の `work/` で行います。本体の環境変数は、弱い順に worker の起動時の値、呼び出しの値、読み込み中に Card が設定した値（`main.py` の先頭。常駐のあいだ続く）、preRun の export で、Python の `os.environ` も呼び出しごとにこれに揃えます。preRun より読み込みが先なので、ライブラリが読み込み時に読む設定（OpenBLAS や libgomp のスレッド数など）は `main.py` の先頭で import の前に設定します。SDK は numpy を読み込む `flatbuffers` を先に import しないので、これに間に合います。worker の起動はプールの長命なスレッドで行います（bubblewrap の `--die-with-parent` は起動したスレッドの終了で worker を止めるため）。`filesystem: false` の Card の `work`・`tmp` と、preRun / bash の run / postRun のファイルは worker ごとの VM 内の一時領域（プロジェクトの共有フォルダではない）にあり、呼び出しの間に空にします。`out` は File / Directory の出力を持つ Card にだけ作り、下流と成果物のために残します。sandbox の中から見えるのは、プロジェクトの `runs`・保存済み入力（読み取り専用、起動時に 1 度だけ bind）と自分の一時領域で、呼び出しの出力フォルダは `SCM_RIGHTS` の FD で渡して `/proc/self/fd/900` として見せ、core が本来のパスに戻して検証します。作業フォルダは FD で渡しません（sandbox の外を指すカレントフォルダには `getcwd` が失敗する）。`filesystem: true` の Card も一時領域で動き、呼び出しの後で `work`・`tmp` を run のフォルダへ写します。入力を FD で渡さないのは、`/proc/self/fd/N` から開き直すと読み取り専用を破れるためです（入力は読み取り専用の mount で守る）。Mac の開発環境（bubblewrap なし）は本来のパスをそのまま使います。
- **bash の入力**: ポート `P` ごとに `JSON_P`（JSON ファイルのパス、大きさの制限なし。`jq` が使える）、File/Directory は `FILE_P`/`DIR_P`。入力の JSON とその変換器は bash の `run` を動かすときだけ作ります。出力は `$TSUGUMI_OUTPUT` の JSON をポートごとに型検証します（成果物の `out` とは別なので、成果物のない Card でも動く）。
- **Nix の上流参照**: `default.nix` は `inputs.upstream."<Card path>"`（`inputs.local.<id>` は同じ階層）で、**どの経路で発火しても必ず先に実行される**コード Card の属性集合（`run`・`preRun`・`exports` の関数や値・store context）を参照できます。同じポートへの複数経路は共通部分、`all` は必須入力の和、`any` は全入力の共通部分、ループは最大不動点で求め、分岐の片側だけの Card や下流・自分自身は渡しません（静的診断 `nix_reference_*`）。参照されたものだけが遅延評価され、非選択分岐は build しません。Nix に渡すのはソース（`_card.json`・`_meta.json` を除く）・入力型・Nix 用 params・参照する上流定義で、実行時だけの params は含みません。Tsugumi 独自のビルドキーやキャッシュの記録はなく、評価した derivation の結果（outPath）が store にあれば build しません。評価器は tsugumid に 1 つで、nixpkgs の読み込み・評価状態・daemon 接続を Card と実行をまたいで使い回します（取消・`buildTimeoutSeconds` は評価器のスレッドだけを止める）。
- **Card in Card**: 子 Workflow は `boundary.inputs/outputs` で外側のポートを内部 Card のポートに対応づけ、外からは通常の Card として接続・実行できます（Run の対象にでき、単体実行では境界入力に保存済みの値を使う）。状態と履歴は完全な Card path で区別します。見た目だけのグループ（layout.json）とは別物です。
- **概要**: Card の概要は種類によらず `_meta.json` の `description` だけに持ちます（Card in Card も同じ。`_card.json` には持ちません）。`_meta.json` は Nix の評価に入らないので、書き換えても再ビルドは起きません。
- **Draft**: `_meta.json` に概要・処理内容・制約・完了条件・入出力の用途を持ちます。ポートは線をつなぐと自動で作られ（入力は `_card.json`、出力は `_meta.json`。最後の線を切ると消える）、型なしのまま配線できます。実行も成功扱いもせず、Draft に依存する Card は理由付きで blocked、独立した部分はそのまま実行します。「Card を生成」は Agent に Draft・配線・説明を渡し、PutDefinition/PutSource の 1 トランザクションで型・Nix・ソースを保存してから検証・実行させます（途中の失敗や取消では何も保存されず、計画は残ります）。Agent の保存（`origin` が `agent`）で Draft が Draft でなくなるとき、`Service::save` は `_meta.json` を検査し（`permissions::incomplete_draft_intents`、Card in Card の中も）、`description` が空、Draft に `description` か `content` があったのに `context` が空、Draft にあった `description`・`content`・`context` が変わった、のいずれかなら `validation_failed` で何も保存しません。文面の中身（書かれていないことを足さない）はプロンプトの約束で、検査できるのは空かどうかと変わっていないかだけです。Draft を消して別の id の Card に置き換える保存は対象外で、人間の保存も対象外です。
- **Agent**: `agent/` の独立パッケージ。tool 定義は `tsugumi.bfbs`（FBS の doc comment 付き）から生成し、tool 引数⇄FlatBuffers も同じ BFBS で変換します（64 bit は 10 進文字列）。op_id は tool 呼び出しごとに Agent が付け、会話・tool 証跡と一緒にホスト側へ永続化します。応答を失った変更は OperationGet で照合し、盲目的に再送しません。tool の結果の中の PNG と JPEG（`run_record` の画像表示、ファイルや成果物の読み取り）は、文字列ではなく画像としてモデルに送ります（`ToolImages.swift`、Web は `splitImages`）。途中までしか読めていない画像や大きすぎる画像は送らずに一言添え、プロバイダが画像を受け付けなければ、その会話の画像を外して送り直します。Python・C++・bash の Card に必要なことは指示に書いてあり、モデルは参照を読まずに書き始めます。Python SDK のメソッドの一覧は `agent/Shared/generate.py` が SDK のソースから作って指示に入れます。`reference_read` は Card のフォルダなら中の全ファイルを、ほかのフォルダなら配下のファイルの一覧を 1 回で返します。モデルの API キーは Keychain に保存し core/worker には渡しません。取消はモデル通信と未送信 tool を止め、受理済み run は止めません。

## リリース

- **版番号**は `VERSION` だけにあり（core・Nix・CMake・アプリが読む）、v1.0.0 までは 0.x で、版の間の互換性は保ちません（[AGENTS.md](https://github.com/gen740/Tsugumi/blob/main/AGENTS.md)）。
- **リリースの作り方**: origin/main を取り出したチェックアウトで `macOS/tools/release.sh 0.2.0` を実行すると、`VERSION` を書き換えて commit し、タグ `v0.2.0` とともに main へ push します。チェックアウトなしなら `gh workflow run release.yml -f version=0.2.0`（Actions が同じことをする）。どちらもリリースの workflow（`.github/workflows/release.yml`）がアプリをビルドして GitHub Release を作り、インストール済みのリリース版はそれを見つけて自動で更新します（`macOS/Sources/App/Model/Updater.swift`）。
- **VM の起動ディスク**（`vm-boot.nix`・nixpkgs の固定）を変えたときは、リリースの前に `macOS/Container/build-image.sh --vm` と `macOS/tools/publish-vm.sh`（ディスクと一緒にライセンスの一覧と対応ソースも公開します）で [gen740/tsugumi-vm](https://github.com/gen740/tsugumi-vm) に公開します。ランナーは Apple container を使えないので、公開版がないとリリースのビルドは失敗します。
- **開発版とリリース版**（`AppChannel.swift`）: `build-app.sh` は既定で開発版（`Tsugumi Dev`、bundle id `app.tsugumi.Tsugumi.dev`、`Info.plist` の `TsugumiChannel` が `dev`）を作り、`--release` でリリース版（`Tsugumi`、`app.tsugumi.Tsugumi`、`release`）を作ります。サポートフォルダ（VM・store・control フォルダ・会話履歴）は開発版が `~/Library/Application Support/Tsugumi-dev/`、リリース版が `Tsugumi/` で、利用者の設定（UserDefaults）も bundle id ごとに別です。開発中の確認やテスト（UI スクリプト・smoke・`swift test`）は開発版で行うので、使っているリリース版の VM には触れません。Python のテストの共通 VM は `build/test-vm` です。
- **開発版の署名**: 既定ではどちらも ad-hoc 署名です。ad-hoc だと署名の条件（designated requirement）がそのビルドのバイナリのハッシュになり、ログインキーチェーンは再ビルドのたびに別のアプリとみなして（API キーの読み取りなどで）許可のダイアログを出します。開発版は、この Mac のキーチェーンにある証明書（Apple Development など）を環境変数 `TSUGUMI_CODESIGN_IDENTITY` に指定して署名できます: `TSUGUMI_CODESIGN_IDENTITY="Apple Development" macOS/build-app.sh`（`codesign --sign` が取る証明書の名前の一部か SHA-1。使える証明書は `security find-identity -v -p codesigning`）。署名の条件が証明書と bundle id になり、再ビルドしても変わりません。`--release` では環境変数を無視して ad-hoc のままです（リリースの成果物は環境で変わりません）。ad-hoc から切り替えた後の最初の 1 回は、キーチェーンのダイアログが出ます（「常に許可」を選ぶか、キーを保存し直してください）。以後は出なくなります。VM に要る `com.apple.security.virtualization` の entitlement は、どちらの署名にも付きます。
- **自動更新**はリリース版だけが行います。起動の 20 秒後と 6 時間ごとに `api.github.com/repos/gen740/Tsugumi/releases/latest` を読み、今より新しい版（draft・pre-release は除く）に `Tsugumi.zip` と `Tsugumi.zip.sha256` があればアプリと同じボリュームの一時フォルダにダウンロードし、チェックサム・`codesign --verify --deep --strict`・bundle id・`TsugumiChannel`・版番号を確かめます。入れ替えは `applicationWillTerminate`（VM を止めたあと）で `FileManager.replaceItemAt` により行い、「再起動して更新」のときはこのプロセスが終わるのを待って新しいアプリを開きます。UI スクリプトと smoke の実行中は確認しません。

## 文書サイト

[GitHub Pages](https://gen740.github.io/Tsugumi/) で読む文書は、`docs/` の Markdown から生成します。リポジトリの設定で、Pages の公開元を `main` ブランチの `/docs` にします。

- **生成**: `node docs/site/build.mjs`（`web/node_modules` の marked を使う）で `docs/index.html` と `docs/pages/*.html` を作ります。手で編集しません。一覧とグループは `docs/README.md` から作ります。文書や図を直しながら見るときは `node docs/site/serve.mjs`（http://localhost:4100/）で、変更のたびに生成し直して開いているページを更新します。
- **AI 向け**: 同じ生成で、各ページの Markdown 版（`docs/pages/*.md`、README は `docs/index.md`。リンクはサイト上で切れないよう書き換える）と、それらの案内 `docs/llms.txt`（[llms.txt](https://llmstxt.org/)）を作ります。HTML は `rel="alternate"` で Markdown 版を、`rel="describedby"` で `llms.txt` を指します。静的配信なので、`Accept: text/markdown` での出し分けはしません。
- **動く図**: `docs/site/js/` の Canvas 2D です。アプリの「Tsugumi を知る」は同じページを `WKWebView` で表示するので（`macOS/Sources/App/Explainer/ExplainerWindow.swift`、`build-app.sh` が物語のページとその素材を同梱）、文章と図の正本はここだけです。Home のギャラリーと「はじめての Card」のデモは `docs/site/player/`（Web UI の Card をそのまま使い、実際の実行の記録を再生するもの）で、`node docs/site/player/build.mjs` で作り直します（`web/src` の Canvas・Card のコード、`web/src/styles/`、`demos/`、`narration.json` を変えたら、`docs/site/js/player.js`・`player.css`・`player-data/` を作り直してコミットします）。アプリと Web UI の Home のデモのサムネイルも同じデータ（`docs/site/player-data/`）を描きます。Web UI はプレイヤーそのもの（`web/src/ui/demoPlayer.tsx`）、アプリは自分の Card と線の部品です（`macOS/Sources/App/Home/DemoThumbnail.swift`）。図を入れる位置は `build.mjs` の `STORY` に文の一部で書き、その文がなくなると生成が失敗します。
- **生成物は手で編集せず、作り直しを CI が確かめる**: 文書のページは `node docs/site/build.mjs`（`--check` で差を調べる）で作ります。`test_docs.py` が、生成物が文書と一致することと、未解釈の `**` が残っていないことを確かめます。プレイヤー（`docs/site/js/player.js`・`player.css`・`player-data/`）は `node docs/site/player/build.mjs`、確かめるのは `node docs/site/player/build.mjs --check` で、`web/node_modules` と `web/src/gen` が要ります。CI の Web のステップがこの `--check` を実行するので、`web/src/model/canvasKit.ts` などを変えて作り直し忘れた PR は落ちます（生成は決定的で、同じ入力から同じバイト列になります）。
- **JS なし**: 本文は静的な HTML なので読めます。動きを減らす設定では、図はスクロールに追従し、時計で動く部分は止まります。

## テスト

対象ごとのテストファイルと確認内容を示します。件数と実行結果は各実行のレポートを参照してください。

| テスト | 内容 |
| --- | --- |
| `tests/python/test_contract.py` | §6.7 の往復例、同時保存で片方だけ成功、同一 op_id の同時再送・再起動後再送、障害注入 3 点、重複キー/未知キー/不正 ID、uint64 最大値と表記保持、明示 false/0、Hello 規則、未知 union・不正長で切断、二重起動拒否、ソースの hash 競合・UTF-8 境界 patch・分割読取、外部変更と Reload、View の CAS、日本語ログの byte 境界・tail・範囲外、ページングと cursor、SDK 準備と失効、CacheClear、import grant、遅い peer、UI 用 layout 項目と Draft、整形（JSON/C++/Python/Nix・構文エラー位置・formatter 不在・保存しない）、Hello の版の範囲と capabilities（Display の種類・formatter）、`Failed` 以外の例外でも操作を終えて同じ id で再送できること、保存 journal の圧縮（8 MiB 超）後も受領記録で同じ答えを返し、大きな保存が続いても圧縮が保存ごとに走らないこと |
| `tests/python/test_smoke.py` | いちばん短い往復: Hello → Open → 保存 |
| `tests/python/test_regressions.py` | 報告された不具合の再現: 履歴保存失敗で run が成功にならない、File の fork 転送、CacheClear・再起動後も単体再実行の保存入力（File 実体を含む）が使える、保存入力の不足は明示的に失敗、run のどの瞬間に取り消しても tsugumid が動き続けること、名前の変わった Card に表示がついていき、空いた名前を使う新しい Card には古い表示が届かないこと（同じ中身で戻った Card には戻る） |
| `tests/python/test_scheduler.py` | any の到着順消費・欠落/重複なし、all の 1 値ずつ消費と optional 出力の非発火、maxRuns、Card in Card の単体実行・伝播・同じ内部 ID の path 区別、保存入力なしの明示的失敗、分岐の 1 つからの実行が合流で保存入力と出会うこと（同じポートの新しい値が優先、`any` は足さない）、Start→入力 Card と Draft 依存の blocked |
| `tests/python/test_history.py` | `recordIO × retainArtifact` 全組合せ×CacheClear×再起動、成果物の改ざん検出・パス逸脱、大容量のページング、UTF-8 境界、過去と最新の記録の分離 |
| `tests/python/test_run_history.py` | 履歴を別の場所に置いたとき（`--history-dir`）: `.tsugumi/runs.sqlite` への書き出し（形式・値と bfbs・ログ・成果物・rollback journal）、コピーしたプロジェクトでの読み込み（記録・成果物）と `run_id` での統合、`saveRunHistory` のオフとオンへの切替での追いつき、日時指定と全部の削除（両方の DB と実行のファイル、受領記録での再送、再起動後も戻らない）、形式の版が違う履歴は作り直すこと、古い版の履歴は移行し（コピーを残す）新しい版の履歴は拒否して残すこと |
| `tests/python/test_transport.py` | WebSocket の Origin/subprotocol/token 拒否、fragment・ping 割り込み・連結、Unix の 1 byte ずつ・連結フレーム、実行元切断後も run 継続・別接続から状態取得、Hello 前の peer が大きなフレームを流しても溜め込まずに切断すること |
| `tests/python/test_cards_container.py` | 実環境（共通 VM）: C++ SDK（`card.hh` の `run(Card::Input&) -> Card::Output`、merge union・任意出力・File 出力・params・visualization、package の `run`、再利用 worker でも invocation ごとの preRun/postRun、2 つの SDK が同じ共通 runtime を使うこと）、背圧で進行不能な run の終了、bash 契約（preRun の cd/環境、preRun・postRun に入力がないこと、300 KB 入力は JSON ファイルだけで `.bin`・`INPUT_` なし、`jq`、期限の継続、post 失敗）、取消で子孫プロセスまで終了（sandbox on/off）、隔離（プロジェクト・token・ネットワーク）、Nix 上流参照と拒否、段階とキャッシュ、Python の表示 4 種、Python の Card の環境変数（preRun の export と `main.py` の先頭で設定した値が `os.environ` と C の環境の両方で `main` に届き、常駐の次の実行でも同じこと）、Python/C++/shell 間の値（64 bit 境界・10 万要素・enum/struct/union・別名前空間・File・破損 buffer・必須出力欠落） |
| `tests/python/test_shared_vm.py` | 共通 VM: 履歴が VM のディスク（`/history`）にあり共有フォルダにないこと・`.tsugumi/runs.sqlite` への書き出し・再起動後も残り、データ削除後は `.tsugumi/runs.sqlite` から戻ること、2 プロジェクトが 1 つの Nix store を共有しつつ uid・見えるファイル・プロセス（PID 名前空間）が分かれていること、片方を止めてももう片方の実行が成功し、止めた方も同じ VM で再開できること、ビルド結果が開き直し・共有 store の掃除・プロジェクトのコピー（コピー元のデータ削除後も）でビルドし直さずに使えること、GC root のフォルダを他のプロジェクトが作れないこと、閉じたプロジェクトがフォルダの slot を返すこと（slot 数より多く順に開閉）、VM の中で落とした tsugumid が次の起動で同じ文書のまま立ち上がること、vmd が不正な要求（パスを含む ID・語数違い・未知の語・長い行）を 1 つずつ拒否して動き続けること |
| `tests/python/test_workers.py` | 共通 VM: 常駐 worker が次の実行でも同じプロセスで動き `onRunStart` で実行が始まること、File 入力の Card が worker を使い回して状態（stream）を保ち、入力に書き込めないこと（出力は FD のフォルダに書け、渡していない FD はないこと）、`reuseWorker = false` が呼び出しごとに新しい worker になること、評価の無限再帰・`buildTimeoutSeconds` の後も tsugumid が動き続けること、`network` を許可しない Card は sandbox の有無にかかわらず preRun・run・postRun のどこからも外へ通信できないこと、`hostFolders` のフォルダが指定した Card にだけ見え `write: true` 以外は読み取り専用であること |
| `tests/python/test_mpl_backend.py` | Python SDK の matplotlib backend（代わりのホスト module で）: backend の既定、`plt.show()` の 1 枚の画像と図を閉じること、開いたままの図、複数の図の縦積み、`plt.pause()` のライブフレーム（等倍）と最後の 2 倍の画像、`FuncAnimation`（blit なし）の再生、停止で終わる無限アニメーション、失敗した呼び出しで図が残らないこと、Card 自身の表示を閉じ忘れの図で上書きしないこと、Card が選んだ backend を尊重すること |
| `tests/python/test_plots.py` | Python SDK の Plotly / Altair（代わりのホスト module で）: `fig.show()` が図の JSON を `plotly` の module 表示で送ること、`plotly.io` が先に import されていても設定されること、`chart.show()` がデータ込みの Vega-Lite を `vega-lite` で送ること（IPython もブラウザも要らない）、`ctx.display_figure` が両方を受けること、1 MiB を超える図は理由付きで失敗すること、開いたままの matplotlib の図で上書きしないこと |
| `tests/python/test_demos.py` | 一覧（`demos/index.json`）のすべてのデモが開いて解決できること（実装済みは診断なし、Draft 版は Draft だけ）、Plots デモの図がライブと最後の画像で表示に届くこと・Plotly と Altair の図が module 表示（図の JSON・データ込みの Vega-Lite）で届くこと、3 デモの完成条件（Collatz は C++ と Python だけの写しで独立実装の参照値と照合、子 Workflow の単体実行、反復上限と整数あふれを含む。Julia と R のビルドは走らせない）、異常終了後の interrupted と再起動、Nix 準備中の状態取得・取消。`TSUGUMI_TEST_CONTAINER=1` で実コンテナ環境に対して実行 |
| `tests/python/test_draft_intent.py` | Agent が Draft を実装する保存: `description`・`context` が空なら拒否し（空白だけも空、材料がなければ `context` は空のまま）、Draft にあった内容の変更も拒否して何も保存しないこと、埋めれば通ること、Card in Card の中も同じこと、承認を求める前に拒否すること、Draft のままの保存・他の Card・人間の保存は対象外であること |
| `tests/python/test_card_comments.py` | Card のコメント: Agent の保存は追加・編集・解決・削除ができ、誰が何をしたかが origin から記録される（モデルの申告は使わない。Card in Card の中も）、名前を変えてもついていく、実行に関わらない、版の履歴に変化が出る |
| `tests/python/test_agent_contract.py` | Swift と Web の Agent が共有する `agent/Shared/`（system prompt・tool にする RPC・Agent 自身の tool と Draft 依頼文）から Swift 側の `Shared.swift` が生成済みであること、Web 側が契約（`tests/fixtures/agent-tools.json` の tool 一覧、`agent-contract.json` の FlatJSON 境界値と Draft 依頼文）を満たすこと（`web/scripts/contract.mjs`。node・`web/node_modules`・`web/src/gen` がなければ skip。tool 一覧のスナップショットは RPC を変えたら `cd web && npm run contract -- --update`）、Web UI のサーバーが ChatGPT にサインインしてプランを使うこと（`web/server/chatgptCheck.ts`、`npm run check:chatgpt`: テスト用モデルが OAuth の発行者も務め、ループバックの受け口・ID トークンの検査・0600 の記録・プランの規則・トークンの更新・利用上限・サインアウト） |
| `tests/python/test_claude_code_plugin.py` | Claude Code の plugin の MCP サーバー（`web/dist/mcp.js`）: Web UI のサーバーがなければ起動して `server.json` で見つけ直すこと、プロジェクトを開いてチャットの Agent と同じ道具を実行し、ページが各呼び出しを受け取ること（鳥が Card へ飛ぶ）、開く前は何も動かないこと、`server.json` の鍵は plugin の使う口にしか届かず承認に答えられないこと（node・`web/dist`・tsugumid が必要） |
| `tests/python/test_agent_tools.py` | Agent が頼る RPC の振る舞い: `at_rev` なしの読み取りは最新、`""` はプロジェクトのフォルダ、`project_validate` は保存せずに検証、入力 Card がプロジェクトのファイルを File として渡すこと |
| `tests/python/test_definitions.py` | 定義は Card ごとに 1 つ（名前 = ID = 定義フォルダ）、改名でフォルダと実行履歴が移ること、Card in Card は `externals/` の単独でも開けるプロジェクトで外側から編集すること、trigger |
| `tests/python/test_versions.py` | 版の管理: プロジェクトは git のリポジトリで、実行は保存内容を（変わっていれば）commit してその commit を記録し、結果と実行した Card の概要は commit の note に、名前を付けた版は annotated tag に、版の要約は別の note に、版の差分は要約に読ませてよいファイルだけに、古い版から始めると branch になること、デモの履歴ファイルが版になること |
| `tests/python/test_permissions.py` | モデル（origin `agent`）の保存が Card の権限（ネットワーク・sandbox off・ホストのフォルダ、Card in Card の中も）を広げるときは `approval_required` で拒否され、その広がりを承認として付けたときだけ通ること。利用者自身の保存には承認が要らないこと |
| `tests/python/test_untrusted_project.py` | 他人から来たプロジェクトの `.tsugumi/`（保存 journal・実行履歴のコピー）を使う前に検査し、プロジェクトの外を書き・読み・消しできないこと |
| `tests/python/test_host_isolation.py` | ホストの tsugumid（Linux は bubblewrap、Mac は Seatbelt）で sandbox の Card が、このホストのループバックのポートにつながらず、自分の領域の外のファイルを読めず、ホストのファイルを書き換えられないこと（CI の Linux・macOS ジョブ。文書だけの変更では macOS を省く） |
| `tests/python/test_host_sandbox.py` | Mac 上の tsugumid（Web UI）が sandbox の Card を Seatbelt で隔離し、ネットワークを許した Card は名前を解決できるが、このホストのどのアドレス（loopback・LAN・`0.0.0.0`）にも、`mDNSResponder` 以外の Unix ソケットにも、`trustd` 以外の Mach サービス（クリップボード・LaunchServices など）にも届かないこと。自分の出力フォルダにだけ書けること。Python の Card が sandbox の中でも読み込み中に現在のディレクトリを尋ねられ、`main` に環境変数が届くこと |
| `tests/python/test_web_server.py` | Web UI のホストサーバー: 表示した URL の key（その後は cookie）を持つページだけが `/api/`（`/api/settings` を含む）に届くこと、変更には cross-origin から送れないヘッダーが要ること、不正な要求はそれだけが失敗しページの外のファイルを返さないこと（node と `web/dist` が必要）。グラフの表示部品（`/modules/`）だけがこのページからの埋め込みを許され、常に sandbox（opaque origin）で、何も読み込まず（スクリプトは埋め込み、ライブラリはページが渡す）どこにも接続しないこと |
| `tests/python/test_frame_fuzz.py` | 固定 seed の fuzz: Unix と WebSocket のフレーム（任意のバイト列・偽の長さ・任意のヘッダー bit・マスクなし・順不同の fragment と制御フレーム・Hello の前後でバイトを書き換えた要求）で tsugumid が落ちず止まらず、ほかの接続に正しく答え続けること（`TSUGUMI_FUZZ_CASES` で件数、既定 300） |
| `tests/python/test_en_strings.py` | 英語 UI の訳が原文と同じ引数を同じ順に取ること（順を変える訳は `%2$llu` のような位置指定）、同梱の `Localizable.strings` が生成済みであること。ビルドが出す文字列のうち訳のないものは `en_strings.py --check <stringsdata のフォルダ>` で一覧できる |
| `tests/python/test_demo_mnist.py` | MNIST のデモ（`TSUGUMI_TEST_DEMO_RUNS=1` のときだけ実行。Nix の取得が大きいため CI では動かしません）: 公表されている数字ごとの枚数、numpy だけの kNN と Card の一致、再実行で正解率が変わらないこと、1 つの分岐だけの実行と、そこから `compare` へ進めての比べ直し。`mnist-classifiers-draft` に完成版の Card を保存すると実行でき、Draft に書いた正解率の下限を超えること |
| `tests/python/test_demo_mnist.py` | MNIST のデモ（`TSUGUMI_TEST_DEMO_RUNS=1` のときだけ実行。Nix の取得が大きいため CI では動かしません）: 公表されている数字ごとの枚数、numpy だけの kNN と Card の一致、再実行で正解率が変わらないこと、1 つの分岐だけの実行 |
| `tests/python/test_demo_python_numerics.py` | Newton 法・薬物動態・最大流のデモ（`TSUGUMI_TEST_DEMO_RUNS=1`）: 厳密な根、閉じた式、総当たりのカットとの照合、1 つの Card だけの再実行 |
| `tests/python/test_demo_python_lab.py` | 濃度–反応曲線・分子の物性・細胞画像の計数のデモ（`TSUGUMI_TEST_DEMO_RUNS=1`）: 合成データの真の値、分子式から求めた分子量、正解の位置との照合、1 つの Card だけの再実行 |
| `tests/python/test_language_cards.py` | R と Julia の Card（`rCard`・`juliaCard`）: params・値・File 出力が言語をまたいで型のとおりに渡ること、各種の表示が記録に届くこと、スクリプトの失敗が Card の失敗になること。bash Card の `$TSUGUMI_DISPLAY`（不正な内容は理由つきで失敗） |
| `tests/python/test_demo_r.py` | R の 4 デモ（`TSUGUMI_TEST_DEMO_RUNS=1`）: 手で書いた Kaplan–Meier と Cox、逆分散重みの併合、既知の応答遺伝子、Pearson の相関との照合、1 つの Card だけの再実行 |
| `tests/python/test_demo_julia.py` | Julia の 4 デモ（`TSUGUMI_TEST_DEMO_RUNS=1`）: 総当たりとの照合、定常分布の閉じた式、熱の保存とモードの減衰、RK4 の保存量と次数、1 つの Card だけの再実行 |
| `tests/python/test_demo_collatz.py` | Collatz の 4 言語（C++・Python・Julia・R）のデモ（`TSUGUMI_TEST_DEMO_RUNS=1` のときだけ実行。Julia と R を作り、1 ステップごとにプロセスを起こすので 27 で 3〜6 分かかり、CI では動かしません）: 27 が 4 言語で同じ 112 個の数列になること、R が正確に持てない値（2^53 以上）では R の Card が理由つきで失敗し、Julia は正確な数列を返すこと |
| `tests/python/test_licenses.py` | アプリが同梱する唯一の数式フォントだけを使っていること |
| `tests/python/test_docs.py` | 文書の検査: リンク・アンカー・パスの実在、文書・テスト・デモの一覧の漏れ、`Protocol.md` と静的サイトが生成元と一致すること、[Evidence.md](evidence.md) のテスト名の実在、[CardGuide.md](cardguide.md) のコードをそのページから取り出して Python・bash・C++ で実行すること |
| `tests/python/test_release.py` | 版番号が `VERSION` 1 つから core・Nix・CMake・アプリに入ること、アプリが Swift Build でビルドされ、ビルドしたマシンのパス（パッケージのリソースの場所）を含まないこと、ビルドしたマシンのパス（`#filePath`）が debug ビルドの中にしかないこと、アプリと Web UI が API キーに同じ Keychain 項目を使うこと |
| `tests/cpp`（`tsugumi_core_tests`） | 出力をリンクを辿らずに読むこと、型（catalog・構造的互換・merge union）、型の伝播、Nix 上流参照（直線・分岐+any・all と optional・同一ポート複数経路・feedback loop・入れ子・到達不能な循環・診断）、run plan（全体・単体・子 Workflow）、履歴の状態選択と障害注入、送信キュー（順序・seq・遅い peer の隔離）、型付き JSON schema（前方一致する名前・似た名前・エスケープした名前・未知キーのキー照合、重複キー・不正な UTF-8・非有限数・型違反・null、64 bit 整数と数値表記・encode と decode の往復） |
| アプリ smoke（`TSUGUMI_SMOKE_DEMO=<id> TSUGUMI_SMOKE_OUT=<json>`、`TSUGUMI_SMOKE_RERUN=1` で 2 回目の実行も） | アプリ自身のコード経路で デモのコピー→環境起動→接続→配置編集の保存→実行→Display→Undo、付箋・グループ・折りたたみ・出力枠の保存、Draft の仮出力、古い基準の下書きが競合になり上書きしないこと |
| UI スクリプト（`TSUGUMI_UI_SCRIPT=<file>`） | 実ウインドウを操作してウインドウ単位で撮影: 長い日本語名と title/sidebar、sidebar 開閉、ウインドウ縮小、Card 設定（ソース・プレビュー）、未保存編集のままカテゴリ移動と外部更新→競合で内容保持、Draft、設定の Skills→一般、エディター、外観プレビュー、Workflow 設定 |
| `macOS/CoreTests`（`cd macOS/CoreTests && swift test`） | アプリ本体なしでビルドする独立パッケージ: モデル一覧の取得が Keychain の確認を出さないこと（ほかのプログラムが保存した項目は確認なしに失敗する）、JSON モデル、Swift 側の Agent 契約（Web と同じ fixture の tool 一覧・FlatJSON・Draft 依頼文）、Mac のフォルダの設定、Agent × テスト用モデル（`tests/fixtures/model_fixture.py`）× 実 tsugumid（読取→変更→実行→説明、取消、取消後の再試行、切断・出力上限での再試行、空の返答（拒否や思考だけ）を伝えて会話が続くこと、手数の上限と続行、文脈圧縮、文脈の上限（`ContextLimitTests`: 上限を超えるツール結果を切り詰めて続行、圧縮が残す分が閾値に収まること、要約の失敗で止まらないこと、`model_context_window_exceeded`、収まらない会話の案内）、キャッシュの記録、Draft の計画→保存→再読込→実装→検証、生成取消で計画が残る、ChatGPT プランでのサインイン→実行→トークン更新→利用上限→入り直し→サインアウト、Web 検索のオンとオフ・検索ブロックの再送・`pause_turn`）、参考資料の読込 |
| `macOS/Tests/TsugumiAppTests`（`cd macOS && swift test`） | アプリのロジック: Home から入り直したチャット（開いているセッションにつながること、接続を待って回答を再開し生成中と示すこと、停止してすぐ送った回答が「停止」と示されないこと）、モデルメニュー（開くたびの再取得、失敗・空の答え・遅い接続で保存済みの一覧が残ること）、段階とフロー方向の幾何、整列（Web と同じ fixture）、ファイル階層、params の整数表記、Skill の読込、ショートカット、同期、Agent の演出、複数のメインウインドウ（プロジェクトの開き先、ツグミがそのプロジェクトのウインドウにだけ出ること）、自動更新（版の比較、リリースの選び方、チェックサム、入れてよいアプリかの確認、開発版とリリース版のサポートフォルダ） |
| `macOS/Tests/TsugumiBenchmarks`（`TSUGUMI_BENCH=1 swift test --filter TsugumiBenchmarks`） | 描画のベンチマーク（3D 点群の 1 フレーム）。通常のテストでは動かない |
| 実際の API（任意、`cd macOS/CoreTests && TSUGUMI_LIVE_API=1 swift test --filter liveModelChangesRunsAndReports`） | アプリと同じ Keychain の Anthropic のキーで、本物のモデルに値の変更・実行・報告を頼み、ツールのエラーがないことを確かめる（数セント程度かかるため既定では動かない） |
| CI（`.github/workflows/ci.yml`、GitHub Actions） | Linux: 開発シェルで tsugumid をビルドし、C++ のテスト、Web UI の型検査とビルド、ホストの tsugumid に対する Python のテスト（Agent 契約・Web サーバー・fuzz を含む）を流す（Card は bubblewrap で隔離）。macOS（`macos-26`）: ホストの tsugumid と tsugumi-worker をビルドし、`macOS/CoreTests` と `macOS/Tests`（Dawn の renderer はキャッシュ）を流す。共通 VM は hosted runner で Virtualization.framework を使えないため Mac で確認する（`TSUGUMI_TEST_CONTAINER=1`）。文書だけの変更では macOS のジョブを省く（アプリが読む `README.md`・`docs/Philosophy.md`・`docs/Name.md`・`docs/Author.md` を除く） |
| リリース（`.github/workflows/release.yml`、GitHub Actions） | タグ `v<VERSION>` で macOS のランナーが `build-app.sh --release` を作り、署名と Info.plist（リリース版・版番号）を確かめて `Tsugumi.zip` と `Tsugumi.zip.sha256` を GitHub Release に公開する（[リリース](#リリース)） |
| `tests/python/measure_stages.py` | 段階ごとの時間の計測（下記） |

## 計測（2026-11-03、高速化後・共通 VM）

> **Collatz のデモについて。** この節と次の節の「Collatz」は、C++ と Python の 2 言語だった時点のデモ（約 1,340 回の呼び出し）の値です。いまの `collatz` は Julia と R が加わって 2,685 回になり、Julia と R の Card は呼び出しごとにプロセスを起こすので、常駐 worker の効果はこの 2 言語に及びません。1 回の Run は Mac 上の tsugumid・Seatbelt で 270 秒（`docs/site/player/harvest.py`）、隔離なしの 4 言語のテスト（`test_demo_collatz.py::test_collatz_27_in_four_languages`）は 368 秒かかります（共通 VM では測っていません）。CI で動く `test_demos.py` の Collatz は C++ と Python だけの写しなので、数秒のままです（`test_collatz_27` 4.9 s、`test_collatz_limit_overflow_and_skipped` 2.3 s）。`measure_stages.py collatz` は現在のデモを測るので、下の表と同じ条件では比べられません。

条件は下の節と同じ（M3 Max、共通 VM 8 vCPU / 16 GiB、Collatz 1344 invocation、3 回目の実行）。
変更: 履歴の書き込み削減、worker の常駐・待機 worker、VM 内の一時領域、bash 以外で入力の変換をしない、評価器の組み込み。

| 指標 | 変更前 | 変更後 | 読み方 |
| --- | --- | --- | --- |
| Run の所要時間（共通 VM、履歴を実行履歴用ディスクに置いた現在の構成。2〜5 回目） | 4.5〜5.0 s | 0.92〜1.0 s（1 回目 7.9 s） | 同期書き込み 1 回 0.34 ms（`dd oflag=dsync`） |
| 同（履歴をまだプロジェクト内に置いていた段階） | 4.5〜5.0 s | 2.6〜2.9 s | 残りの大半は virtiofs 上の履歴 SQLite の確定待ちだった |
| 同（履歴だけ VM 内の tmpfs に置いた試験） | 1.8 s | 0.8 s | 履歴を VM 内に置けばさらに 3 倍強 |
| 同（試験: 履歴を store ディスクに置いた場合） | | 6.4〜6.6 s | store ディスクは `.full`（確定ごとに F_FULLFSYNC）で、同期書き込み 1 回 9.9 ms（virtiofs は 0.14 ms）。そのため履歴用のディスクを分けた |
| 同（Mac 上の tsugumid、参考） | 1.35 s | 0.43 s | |
| 1 回の受け渡し（C++ 側の直列 670 回） | 6.5 ms | 3.9 ms（tmpfs の試験で 0.55 ms） | |
| C++ Card の実行段階（平均） | 4.6 ms | 0.26 ms | 共有フォルダ上のフォルダ作成・削除と入力ファイルがなくなった |
| 2 回目以降の準備段階（9 Card の合計） | 約 470 ms | 20 ms | 評価器の再利用（評価 1 Card 中央値 2 ms）、`nix-store` 起動とファイル書き出しなし |
| 1 回目（プロジェクトを開いて最初） | 7.8 s | 9.4 s | nixpkgs の読み込みを含む評価（tsugumid ごとに 1 回） |
| tsugumid のメモリ | — | 29 MB → 121 MB（評価後） | 評価器と nixpkgs の評価状態。常駐 worker 9 個は合計 89 MB |
| 共有 store の掃除 | — | 1203 → 712 MiB | root のある Card のコンパイラ・nixpkgs のソースは残る（`keep-outputs`） |

保存段階の記録は確定（commit）の直前で閉じるので、確定待ちは段階の外（終了から通知まで）に現れます。
入力を FD で渡さない理由は実測で確認しました: 読み取り専用で開いた FD を bubblewrap に渡すと、中から
`/proc/self/fd/3` に書き込めて元のファイルが変わり、パスでは読み取り専用の bind で拒否されます。

## 計測（2026-11-03、共通 VM、高速化前）

高速化前の比較用記録です。ここで調べていた履歴保存の待ち時間には、常駐 worker・VM 内の一時領域・履歴用ディスクの分離などで対処しています。変更後の値と条件は[高速化後の計測](#計測2026-11-03高速化後共通-vm)にあります。

条件: MacBook Pro M3 Max（36 GB）, macOS 27.2, 共通 VM 8 vCPU / 16 GiB、Collatz デモ（1344 invocation）。
`tests/python/measure_stages.py collatz`（VM、`--host` で Mac 上の tsugumid）と、アプリの smoke モード
（`TSUGUMI_SMOKE_RERUN=1`）で計測。いずれも 2 回目（ビルド済み・worker 再利用）の値です。

| 指標 | 共通 VM | Mac 上の tsugumid | 読み方 |
| --- | --- | --- | --- |
| Run の所要時間（core の記録） | 4.6〜4.8 s | 1.3 s | 差の大半は下の「保存」 |
| 保存段階（履歴 SQLite・成果物の記録） | 合計 7.9〜8.1 s、中央値 6 ms | 合計 0.64 s、中央値 0 ms | VM ではプロジェクトフォルダが virtiofs 越し。同期書き込み自体は 1 回 0.14 ms（`dd oflag=dsync`）で、fsync が主因ではない。SQLite の WAL・ロックとファイル操作の往復が候補（未断定） |
| 実行段階 | 合計 2.9〜3.2 s、中央値 1 ms | 合計 1.0 s、中央値 1 ms | worker は再利用（`build/reused`・`evaluate/reused` 662 回、0 ms） |
| 評価・ビルド | キャッシュ（0 ms） | キャッシュ（0 ms） | 実行のたびに SDK 準備・build を繰り返していない |
| 終了から通知まで | 中央値 8.3 ms、最大 22 ms | 中央値 3.8 ms、最大 6 ms | VM の時計は 30 秒ごとに Mac に合わせる（合わせる前は 0.38 s 進んでいた） |
| core の終了 → アプリが終了を表示 | 20〜60 ms（Collatz 4.63 s → 4.68 s） | — | core は UI を待たない。アプリは 1.3 万件の通知を 262〜325 回にまとめて反映 |
| アプリ起動 → 操作可能（VM 起動を含む） | 6.2〜9.8 s | — | VM の起動・store の mount・nix-daemon・DHCP・tsugumid |

## 計測（2026-11-02、Apple container 時代）

現在の共通 VM に移る前の記録です。下表のイメージ・プロジェクトごとの store・起動時間は当時の構成に対する値です。

条件: MacBook Pro M3 Max（36 GB）, macOS 27.2, Apple container 0.6.0, 環境 2 vCPU / 4 GiB / store volume 20 GiB,
デモは各 1 回（Live Stream の継続利用は 10 回）。アプリの smoke モード（`TSUGUMI_SMOKE_DEMO`）と
`tests/python` のスクリプトで、実コンテナ環境に対して計測しました。

| 指標 | 実測 | 目標との比較 |
| --- | --- | --- |
| 基本イメージ | OCI 圧縮 93.5 MB / 展開後 `/nix/store` 272 MB（整形ツール astyle・ruff・nixfmt・shfmt を加えた当時の版は圧縮 103 MB。clang-format は LLVM 全体で約 540 MB 増えるため入れていない） | 大きい依存: glibc 47 MB, ICU 42 MB と Boost 18 MB（Nix 本体の依存）, util-linux 29 MB（overlay の mount だけに使用。静的 mount に置換可能）, bash 9.5 MB, OpenSSL 9 MB, SQLite 6 MB, tsugumid 一式 6 MB |
| Nix 依存取得後の store（プロジェクトごとの volume 上層） | 約 816 MB | gcc 280 MB（C++ Card）, 固定 nixpkgs ソース 200 MB（Card の Nix 評価）, Python 146 MB, perl 59 MB・binutils 38 MB（stdenv）。10 回実行での増加 0 |
| 履歴・成果物 | Live Stream 1 回で SQLite 約 5 MB（WAL 込み）、10 回で +8.8 MB。Report の成果物 29 KB | ログと I/O 記録量に比例 |
| 起動（キャッシュ済みイメージ, 新規コピーのデモを開く→操作可能） | 1.5〜3.1 s（コンテナ起動・store volume 作成・Hello・Open を含む） | 目標 10 s 以内を達成 |
| 初回準備（新しい store へのツールチェーン取得＋ビルド＋実行） | Collatz 22.5 s, Report 13.4 s, Live Stream 36.7 s（うち標本間隔の待機 20 s） | 回線速度に依存（cache.nixos.org） |
| 応答（Live Stream 実行中の別接続からの RunStatus、100 回） | p50 2.1 ms / p99 10.0 ms | RPC は 100 ms 以内。この計測に SwiftUI の画面での入力応答時間は含めていない |
| 取消 | RunCancel 応答 8.3 ms、Live Stream の worker 停止→cancelled まで 0.12 s。Nix 準備中の取消 0.11 s（別計測、実コンテナ） | 目標（100 ms / 2 s）を達成 |
| 継続利用（Live Stream 10 回、各 24.2〜24.8 s） | 残存 worker/sandbox プロセス 0→0、store 増加 0、履歴 +8.9 MB（WAL 込み）、tsugumid RSS 12.2→15.0 MB | 当初は終了済み sandbox helper の zombie が 1 回あたり 2 個残っていた（tsugumid が PID 1 で reap しなかった）。tini を PID 1 にして解消し、再計測で 0 を確認 |

## 実装状況

現在提供している機能と、その確認方法をまとめます。自動テスト・実画面の記録・実 API の記録は、上の[テスト](#テスト)と下の[確認範囲](#確認範囲)からたどれます。今後追加する機能は[今後の拡張](#今後の拡張)に分けています。

| 領域 | 状況 |
| --- | --- |
| FBS 契約（全要求・成功型・Failure・通知・worker.fbs）、flatc による C++/Python/Swift binding 生成 | 実装済み |
| tsugumid: [Protocol.md](protocol.md) の全 RPC（36 種。SessionHello、Project*、OperationGet、CardSDK*、Run*、Artifact*、LogRange、View*、OutputTypes、AttachedFolderRead、Version*） | 実装済み（契約テスト）。Format は JSON・C++（clang-format、イメージでは astyle）・Python（ruff）・Nix（nixfmt）・shell（shfmt）、FBS は `unsupported` |
| 実行段階の通知・キャッシュ表示、Nix 上流参照、Card in Card の実行、Draft のポートと blocked 理由、表示（text・scalar・折れ線・表・画像・3D 点群・Canvas module） | 実装済み（C++/Python/コンテナのテスト） |
| 保存 transaction・journal・receipt・障害復旧・二重起動防止 | 実装済み |
| スケジューラー（all/any、有界 queue、背圧、maxRuns、skipped/blocked、並列 1〜64、取消、期限、再起動時 interrupted） | 実装済み |
| 型導出（_outputs.fbs、merge union、File/Directory、構造的互換性） | 実装済み |
| Nix レーン（restricted eval・IFD 禁止・キャッシュ・GC root・取消・期限）、C++/Python/shell Card | 実装済み |
| bubblewrap による Card 隔離、環境変数の非継承、`sandbox:false` の扱い | 実装済み（共通 VM） |
| 共通 VM（Virtualization.framework、読み取り専用 rootfs、共通 Nix store、実行履歴ディスク、プロジェクトごとの uid・名前空間・cgroup、ローカル限定公開） | 実装済み |
| Card ごとのネットワーク・Mac のフォルダ（`network`・`hostFolders`）、チャットへのフォルダ添付 | Experimental（`test_workers.py`・`test_definitions.py`） |
| SwiftUI アプリ（[ui-view.md](ui-view.md) の構成、split view の sidebar とチャット inspector、上から下へ流れる Canvas〔共通の Card 表現・段階表示・グループ・付箋・出力枠・Card in Card の作成〕、Card 設定〔実 Card と同じプレビュー・既定値と上書きの区別・構文強調/整形/エラー位置/生成 _inputs.fbs・外部更新との競合保持・境界ポート〕、Draft 編集と生成、定義 tree、表示 7 種、設定〔Skills の非同期読込・エディター見本・操作できる外観プレビュー〕） | 実装済み。アプリのロジックテスト、smoke、UI スクリプトで確認。[README の実画面](../index.md#画面でたどる実験と検証)と[撮影スクリプト](https://github.com/gen740/Tsugumi/blob/main/macOS/tools/screenshots/readme.ui)を公開 |
| Dawn 描画層（Metal、サイズ変更・高 DPI・フレーム単位の集約・資源解放） | 実装・ビルド済み。点群の描画ベンチマークと [Live Stream の撮影スクリプト](https://github.com/gen740/Tsugumi/blob/main/macOS/tools/screenshots/live-stream.ui)を用意（[確認範囲](#確認範囲)） |
| Agent（provider 設定・Keychain・モデル一覧、streaming、BFBS 由来 tool、引数検証、実 RPC、取消、再試行、会話と証跡の永続化・再開、文脈圧縮、プロンプトキャッシュ） | 実装済み。テスト用モデル（`tests/fixtures/model_fixture.py`）で読取→変更→実行→説明、Draft の実装、取消、取消後の再試行（未回答の tool_use を閉じる）、切断・出力上限での再試行、手数の上限と続行、文脈圧縮、キャッシュの記録を自動テスト。実際の API（claude-sonnet-5-5）での Draft から実行までの記録は [AgentRun.md](agentrun.md) |
| Web 検索（プロバイダの `web_search`、設定でオンとオフ、既定はオフ） | 実装済み（アプリと Web UI）。テスト用モデルに対する自動テストだけで（`macOS/CoreTests` の `WebSearchTests`、`npm run contract`）、**実際の API と ChatGPT プランではまだ確かめていません** |
| ChatGPT プラン（Sign in with ChatGPT: ループバックでのサインイン、ID トークンの検査、Keychain / 0600 の記録、トークンの更新と取り消し、プランの規則、利用上限、「Manage usage」） | 実装済み（アプリと Web UI）。認証フローはテスト用モデルを OAuth の発行者とした自動テスト（`macOS/CoreTests`、`npm run check:chatgpt`）。実際のプランで判明したモデル指定の問題は[修正済み](https://github.com/gen740/Tsugumi/commit/8c107a9)。API キーと ChatGPT の併用・接続ごとのモデル選択も[実装済み](https://github.com/gen740/Tsugumi/commit/14ff8fa) |
| 外部 Logger DB | `sqlite:<name>` のみ（`.tsugumi/logger/`）。他の DB は `unsupported` |
| 版の管理（git・実行ごとの版と結果・版のツリー、`VersionInit`・`VersionMark`・`VersionCheckout`・`VersionGraph`） | 実装済み（[Versions.md](versions.md)、`test_versions.py`） |
| Agent の権限の承認（Card の権限を広げる保存は `approval_required`） | 実装済み（[security.md](security.md#agent-の権限境界)、`test_permissions.py`） |
| ツグミのガイド（`guide_show`・`$demo`）、helper への手分け（`delegate`） | 実装済み（[features.md](features.md#9-agent)、`macOS/CoreTests`・`macOS/Tests` の Guide のテスト、Web の helper の鶫は `web/tests/helperBirds.ts`） |
| Claude Code の plugin | 実装済み（[claude-code.md](claude-code.md)、`test_claude_code_plugin.py`） |
| Web UI（同じ画面・操作・ショートカットをブラウザで。Mac 上の tsugumid、Seatbelt による Card の隔離） | 実装済み（[WebUI.md](webui.md)）。型検査・ビルドに加え、UI の振る舞い、Agent の共通契約、ホストサーバー、Seatbelt の隔離を対象とするテストあり（上のテスト一覧） |

## 確認範囲

確認方法ごとに対象を記載します。UI の自動操作環境に必要な権限や、目視で確認する内容は、機能の実装状況と分けて扱います。

| 確認方法 | 対象と範囲 |
| --- | --- |
| UI スクリプトとスクリーンショット | アプリ内の操作経路で実ウインドウを操作し、Card・子 Workflow・ソース・タイムライン・ガイドを撮影。公開画像と撮影手順は [README](../index.md) と [macOS/tools/screenshots](https://github.com/gen740/Tsugumi/blob/main/macOS/tools/screenshots/README.md)。実マウスによるクリック・ドラッグの E2E テストとは確認範囲が異なる |
| Dawn の描画 | 描画ベンチマークは `TSUGUMI_BENCH=1` で実行。Live Stream の折れ線は `live-stream.ui` で実画面を撮影できる。目視確認の結果は本資料には記録していない |
| Agent と ChatGPT | テスト用モデルでは認証・取消・再試行などの分岐を自動確認。実 Claude API の実行記録は [AgentRun.md](agentrun.md)、実 ChatGPT プランでの修正は上のコミットを参照 |
| CI と共通 VM | CI の対象とコマンドは [ci.yml](https://github.com/gen740/Tsugumi/blob/main/.github/workflows/ci.yml)、実行結果は [GitHub Actions](https://github.com/gen740/Tsugumi/actions/workflows/ci.yml)。共通 VM のテストは Virtualization.framework を使える Mac で `TSUGUMI_TEST_CONTAINER=1` を指定して行う |

## 解決済みの記録

以下は Apple container を使っていた構成で発生し、対処した問題の記録です。現在の実行基盤は上の[共通 VM](#実行基盤)です。

- **VM が終了できず残る問題（Apple container 時代）**: Apple container 0.6.0 の推奨カーネル（6.12.28）では、マウント名前空間が片付けられるとき（Nix のビルド sandbox・bubblewrap・コンテナの終了）に SELinux の inode 解放処理 `selinux_inode_free_security` が NULL を参照して kernel oops になり、そのプロセスが終わらず VM が終了処理に進めませんでした。Tsugumi は SELinux を使わない（Card の隔離は bubblewrap の名前空間）ので、同じ設定から SELinux だけを外した専用カーネル（`macOS/Container/kernel.nix`）を作りました。現在の共通 VM もこのカーネルを使います。変更後、起動→Collatz 実行→停止→削除の 8 回の繰り返し（当時の `tests/python/stress_shutdown.py`）で oops 0 件・残存 0 でした。
- CacheClear の Nix GC が環境自身の実行時パスまで回収し、次の起動で entrypoint が壊れる問題がありました（Apple container 時代の、プロジェクトごとの store）。イメージの実行時 closure を GC root にし、起動時に上層の whiteout を取り除くよう修正しました。現在は全プロジェクトで 1 つの store を共有し、GC root は vmd が管理します（[実行環境の置き場所](#主な設計上の決定)）。

## 今後の拡張

N-Queens・200 Card の大規模デモ、WASM 表示は次段階の拡張として残しています。提供中のデモは [demos/index.json](https://github.com/gen740/Tsugumi/blob/main/demos/index.json)、今できないことの一覧は [Limitations.md](limitations.md) を参照してください。
