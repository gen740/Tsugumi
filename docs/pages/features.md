# Tsugumi の主要機能

Tsugumi は、Card をポートで接続したグラフ（Workflow）を Nix で固定した環境で実行し、どの Card がいつ何をしたかを記録する macOS アプリです（ブラウザで同じ画面を使う [Web UI](webui.md) もあります）。
この文書は、機能ごとに「何ができるか」と「どう実現しているか」をまとめた参照用の一覧です。各節の最後の「確かめる場所」に、その機能を確かめるテストとコードを並べています。

| 知りたいこと | 読む場所 |
| --- | --- |
| なぜこの形なのか | [Q&A](qa.md)、[設計の独自性](novelty.md) |
| 導入・ビルド | [INSTALL.md](install.md) |
| テストの一覧と計測値 | [Development.md](development.md) |
| 隔離と秘密情報の扱い | [security.md](security.md) |
| まだできないこと | [Limitations.md](limitations.md) |

## 1. 全体構成

```text
macOS（ホスト）                                   共通 Linux VM（アプリに 1 台）
 SwiftUI UI ─┐                                     tsugumi-vmd（VM の管理役）
 Agent ──────┼─ FlatBuffers RPC ─ 127.0.0.1 ─ vsock ─ project A: tsugumid ─ worker プール
 外部クライアント┘                                   project B: tsugumid ─ worker プール
                                                   nix-daemon ─ 共通 /nix/store
```

- **tsugumid（core）**：プロジェクトごとに 1 プロセスが立ち上がります。文書の管理・保存、RPC、スケジューラー、Nix 評価、worker 管理、実行履歴の記録を担い、UI がなくても RPC だけで保存・実行・履歴取得が完結します。
- **UI と Agent は対等なクライアント**：どちらも `protocol/tsugumi.fbs` で定義された同一の RPC 契約で core と通信します。Agent は画面の操作を模倣するのではなく、core に直接 RPC リクエストを送ります。Agent を無効にしても通常の操作はすべて機能します。
- **共通 VM**：macOS の `Virtualization.framework` で 1 台の Linux VM を起動し、その内部でプロジェクトごとに名前空間・uid・cgroup を分離した `tsugumid` を動かします（詳細は [security.md](security.md)）。
- **Web UI**：ブラウザから同じ画面を使うときは、`tsugumid` を Mac 上で直接動かし、Card は Seatbelt で隔離します（VM は使いません。[Web UI](webui.md)）。

**確かめる場所**

- RPC の契約: [protocol/tsugumi.fbs](https://github.com/gen740/Tsugumi/blob/main/protocol/tsugumi.fbs)、[test_contract.py](https://github.com/gen740/Tsugumi/blob/main/tests/python/test_contract.py)
- UI と Agent が同じ契約を使うこと: [test_agent_contract.py](https://github.com/gen740/Tsugumi/blob/main/tests/python/test_agent_contract.py)

## 2. Card と Workflow

### Card の種類

| 種類 | 役割 |
| --- | --- |
| コード Card | `default.nix` で実行環境を、`src/` で処理を定義する（C++ / Python / bash / R / Julia） |
| trigger | Workflow の実行起点（出力ポート `out`） |
| input | `_outputs.fbs` の型に沿った入力値を Canvas 上で直接編集して流す（File 型は選択したファイルを run にコピー） |
| fork / branch / merge | 分配・条件分岐（`condition` に応じて `a` / `b` の片側のみ発火）・合流（入力ポート名付きの union 型）。有限のフィードバックループ専用の Card はなく、merge で値を戻して作り、ループ上の Card の `maxRuns` で反復の上限を決めます |
| logger | 受け取った値を外部 DB に記録する（現在は `.tsugumi/logger/` 配下の `sqlite:<name>` に対応） |
| subgraph（Card in Card） | 子 Workflow を 1 つの Card として扱う。`boundary` で外側のポートを内部の Card に対応づける |
| Draft | 名前と内容をカードに直接書き、線を落とすだけでつなげる「未実装の Card」（ポートは線に合わせて作られ、型はない）。後から Agent に実装を生成させられる |

### 型付きのポート

- 出力ポートからの線は 1 本です。1 つの値を複数の Card に送るときは fork Card を通します（同じことを書く方法を 1 つにし、Canvas で分配が Card として見えるようにするため）。2 本以上の線が出ている出力は `fan_out` として報告され、実行できません。Canvas で使用中の出力からもう 1 本引くと、fork が自動で挟まります（出力が fork や fork につながっているときは、その fork に枝が増えます）。
- 出力ポートの型は FlatBuffers スキーマ（`_outputs.fbs`）で宣言し、下流の入力ポートの型は上流から自動的に導出されます。配線時に構造的互換性を検査し、merge では自動的に union 型が構成されます。
- 値は FlatBuffers のバイナリ列のまま Card 間を流れます。C++ / Python / bash（R と Julia は JSON に変換して）の間で 64 bit 整数・enum・struct・union・File / Directory を欠落なく受け渡せることを自動テストで検証しています。

### 実行の意味論（スケジューラー）

- 入力の待ち合わせ方には `all`（すべての必須入力が揃ったら 1 値ずつ消費）と `any`（到着順に消費）があります。
- 有界キューと背圧（バックプレッシャー）、並列度 1〜64、キャンセル、タイムアウト期限（呼び出し全体で 1 つ）、反復上限（`maxRuns`）を備えています。
- 条件分岐で選ばれなかった経路の下流は `skipped`、Draft や失敗した上流に依存する Card は理由付きの `blocked` となり、それらと独立した経路は止まらずに実行されます。
- 異常終了や再起動で途中だった実行は `interrupted` として記録されます。
- 「ここから下流を実行」や「この Card だけ実行（最後の入力）」による部分実行では、前回保存された入力（File の実体を含む）をそのまま再利用します。「ここから下流を実行」で合流に届くと、すべての入力を待つ Card は今回届かないポートだけを保存された入力で埋めて実行します（`any` の Card は届いた値だけで動きます）。

**確かめる場所**

- 型・構造的互換・merge の union: [tests/cpp](https://github.com/gen740/Tsugumi/tree/main/tests/cpp)
- 言語をまたいだ値の受け渡し: [test_cards_container.py](https://github.com/gen740/Tsugumi/blob/main/tests/python/test_cards_container.py)、[test_language_cards.py](https://github.com/gen740/Tsugumi/blob/main/tests/python/test_language_cards.py)
- `all` / `any`・`maxRuns`・Card in Card・部分実行: [test_scheduler.py](https://github.com/gen740/Tsugumi/blob/main/tests/python/test_scheduler.py)

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

- `pkgs` は既定で同梱の固定 nixpkgs（`tsugumi:default`）を指します。任意の nixpkgs を `https://github.com/NixOS/nixpkgs/archive/<rev>.tar.gz#sha256-…` の形式でリビジョンとハッシュ付きで固定することもできます。
- Nix の評価には restricted eval・IFD（Import From Derivation）禁止を適用し、取得先 URL を nixpkgs アーカイブのみに制限しています。
- Tsugumi 独自のビルドキャッシュ管理は持たず、「評価した derivation の出力パスが `/nix/store` に存在すればビルドしない」という Nix 本来の仕組みだけで一貫して判定します。

### 上流定義の参照（`inputs.upstream`）

- `default.nix` から、**どの経路を通って発火しても必ず先に実行される**上流 Card の定義（`run`・`preRun`・`exports` の関数や値）を参照できます。
- 「必ず先に実行されるか」の判定は、`all` / `any`・複数経路・ループ（最大不動点）・入れ子構造を考慮した静的解析で行います。分岐の片側にしか存在しない Card や下流、自分自身は参照できず、静的な診断エラー（`nix_reference_*`）となります。`inputs.local.<id>` は同じ階層の Card だけを指します。
- 静的な診断は書かれた参照（`inputs.upstream."<path>"`・`inputs.local.<id>`）を評価前に知らせるためのもので、コメントと文字列の本文（`''…''` の中のシェルスクリプトなど）は読みません（文字列の中の `${…}` は読みます）。別名や `${…}` で組み立てた名前などの間接的な参照は静的には調べず、評価時に判明します。参照できる範囲そのものは、評価器が `inputs.upstream` に必ず先に実行される Card だけを渡すことで保証しています。
- 実際に参照された定義だけが遅延評価され、実行時に選ばれなかった分岐はビルドされません。

### 外部の Nix ソース（`inputs.sources`）

- 評価中に取得できるのは、既定では nixpkgs だけです。プロジェクトは `tsugumi.json` の `nixSources` で、nixpkgs の外にある tarball を名前・URL・hash で宣言できます。`tsugumi.json` を手で書くか、Agent に頼みます（Agent の保存は承認を待ちます）。アプリに編集する画面はありません。

```json
"nixSources": [{ "name": "ciao", "url": "https://github.com/gen740/ciao/archive/<commit>.tar.gz", "hash": "sha256-..." }]
```

- `default.nix` からは `inputs.sources.<name>` で読みます。実際に参照されたときに、hash で固定して取得します。

```nix
{ pkgs, inputs, params }:
let ciao = import "${inputs.sources.ciao}/src/package.nix" { inherit pkgs; };
in { run = "${ciao}/bin/dmlist ..."; }
```

- 宣言が効くのは、そのプロジェクトの Card だけです（Card in Card は別のプロジェクトです）。許可するのは宣言した URL そのものなので、同じリポジトリの別のコミットも取得できません。

### 評価器の組み込み

- `tsugumid` は `libnixexpr` / `libnixstore` をプロセス内に組み込んでおり、nixpkgs の読み込み・評価状態・daemon 接続を Card 間および複数回の実行をまたいで再利用します。これにより、2 回目以降の準備段階は 9 Card 合計で約 20 ms に収まります（[開発資料の計測](development.md)）。
- キャンセルや `buildTimeoutSeconds` が発生した際は、評価器のスレッドだけを安全に停止します。

### ビルド結果の保持

- すべてのプロジェクトで 1 つの `/nix/store` を共有し、プロジェクトごとの GC root でビルド結果を保持します（Card ごとに直近 2 世代、30 日間未使用のものは自動回収）。
- プロジェクトの「ビルドキャッシュを削除」はそのプロジェクトの GC root を外すだけです。共有 store 自体のガベージコレクションは設定の「共有 store を掃除」からのみ実行されます。`keep-outputs = true` を設定しているため、コンパイラや nixpkgs のソースは掃除後も保持され、Card を編集するたびに再ダウンロードすることはありません。

**確かめる場所**

- Card の環境の定義と SDK: [sdk/nix/default.nix](https://github.com/gen740/Tsugumi/blob/main/sdk/nix/default.nix)
- 上流参照の解析: [tests/cpp](https://github.com/gen740/Tsugumi/tree/main/tests/cpp)、[core/src/graph.cc](https://github.com/gen740/Tsugumi/blob/main/core/src/graph.cc)（`compute_guarantees`）
- 共通 store の再利用と GC root: [test_shared_vm.py](https://github.com/gen740/Tsugumi/blob/main/tests/python/test_shared_vm.py)

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
- **R・Julia**（`rCard`・`juliaCard`）：入力と出力は JSON に変換して受け渡します。呼び出しごとにプロセスを起こすので、常駐 worker の効果は及びません。書き方は[はじめての Card](cardguide.md) にあります。
- ポート契約に依存しない機能（可視化・ログ・実行コンテキスト）は共通ランタイム `libtsugumi_card.so` に集約し、Card ごとにビルドするのは型定義と薄いアダプタだけに抑えています。

### 常駐 worker

- `reuseWorker`（既定 `true`）の worker は実行をまたいで常駐し、static 変数などの状態も保持します（各実行の最初の呼び出し前に `onRunStart` / `on_run_start` が呼ばれます）。
- ビルド条件の変更、失敗・キャンセル、10 分間の未使用、メモリ 2 GiB 超過、または常駐数 32 超過で自動的に終了します。
- `reuseWorker = false` の Card に対しては、次回使う新しい worker を裏で先行起動（サンドボックス起動・`.so` 読み込み・Python の import まで完了）させて待機させます。

### 外部へのアクセス（Experimental）

- Card は既定でネットワークを持たず、Mac 側のファイルシステムも見えません（[security.md](security.md)）。
- Card 設定の Experimental セクションで、Card ごとに「ネットワークへのアクセスを許可する」（`network`）および「ホストのフォルダへのアクセスを許可する」（`hostFolders`。worker からは `/host/<name>` として見え、既定は読み取り専用）を個別に指定できます。

**確かめる場所**

- 各言語の SDK と、可視化・File 出力・params: [test_cards_container.py](https://github.com/gen740/Tsugumi/blob/main/tests/python/test_cards_container.py)、[test_language_cards.py](https://github.com/gen740/Tsugumi/blob/main/tests/python/test_language_cards.py)
- 常駐 worker・`onRunStart`・ネットワークと Mac のフォルダ: [test_workers.py](https://github.com/gen740/Tsugumi/blob/main/tests/python/test_workers.py)
- はじめての Card（このページのコードをテストが実行する）: [CardGuide.md](cardguide.md)

## 5. 文書と保存

- プロジェクトは通常のフォルダです。`tsugumi.json`、`definitions/<Card>/`（`_card.json`・`_meta.json`（概要などの意図）・`default.nix`・`src/`・`_outputs.fbs`）、および配置情報の `layout.json` から構成され、そのまま Git で管理できます。
- JSON はすべて型付きスキーマで厳密に検証（UTF-8・重複キー・非有限数・未知キーの拒否）してから扱います。core は受け取った文書テキストを再エンコードしないため、64 bit 整数の表記などもそのまま保たれます。
- **保存トランザクション**：メモリ上で仮適用して検証 → ジャーナル（全ファイル内容と receipt）を `fsync` → 対象ファイルを原子的に置換 → コミット。万一途中でクラッシュしても、次回起動時にジャーナルから再適用されるため、「全件成功」か「全件未適用」のどちらかにしかなりません（障害注入テスト済み）。
- **競合検出**：保存は編集開始時のリビジョンを基準に行われ、他のウインドウや Agent が先に保存していた場合は競合として検出し、無断で上書きしません。また、同一 `op_id` の再送は 1 回だけ適用されます（通信断で応答を失っても `OperationGet` で結果を照合できます）。
- **実行中の保存**：Workflow の実行中は、計算定義（execution fingerprint）を変える保存を `busy` で拒否し、配置変更などのみを安全に受け付けます。

**確かめる場所**

- 保存の往復・競合・同じ `op_id` の再送・障害注入: [test_contract.py](https://github.com/gen740/Tsugumi/blob/main/tests/python/test_contract.py)

## 6. 実行履歴と成果物

- 実行・呼び出し・段階・入出力値・ログ・可視化データ・受領記録をすべて SQLite に記録します。正本は VM の実行履歴専用ディスク（`vm/history.img`）に置かれるため、ホストとの共有フォルダを経由せず高速に書き込め、「すべての実行環境を初期化」しても失われません。
- `recordIO` と `retainArtifact` によって、何を記録・保持するかを Card ごとに選択できます。保持された成果物は改ざんやパス逸脱を検出します。
- `saveRunHistory`（既定 `true`）が有効なら、完了した実行履歴をプロジェクト内の `.tsugumi/runs.sqlite` にも自動で書き出します。入出力値は FlatBuffers のバイナリ列のまま、スキーマ（bfbs）をハッシュで引ける形で保存するため、後から Card の型定義を変更しても過去の実行値を正確に解釈できます。
- プロジェクトをコピーしたり別の Mac へ移したりしても、開いたときに `run_id` で照合して履歴を自動的に取り込みます。
- 「実行履歴を削除」を実行すると、VM 内・`.tsugumi/runs.sqlite`・実行ごとのファイル領域の 3 か所すべてから削除されます。

**確かめる場所**

- `recordIO × retainArtifact` の全組み合わせ・改ざん検出: [test_history.py](https://github.com/gen740/Tsugumi/blob/main/tests/python/test_history.py)
- `.tsugumi/runs.sqlite` の書き出し・取り込み・削除: [test_run_history.py](https://github.com/gen740/Tsugumi/blob/main/tests/python/test_run_history.py)
- 履歴の保存に失敗した実行は成功にならない: [test_regressions.py](https://github.com/gen740/Tsugumi/blob/main/tests/python/test_regressions.py)

## 7. 可視化

- Card のコードから `tsugumi::visualization::text(...)` などの API を呼ぶことで、text・scalar・折れ線・表・画像・3D 点群・Canvas module の出力表示を行えます。
- 折れ線グラフは Dawn（WebGPU 実装、Metal バックエンド）で高速に描画され、Live Stream デモのように実行中もリアルタイムに更新されます。
- Python Card の matplotlib は、SDK の backend（`tsugumi.mpl`）を通じてそのまま Card の出力表示に描かれます。ウィンドウ向けに書いたコードがそのまま動く、X 転送のような仕組みです。`plt.show()` で図を表示し（開いたままの図も呼び出しの終わりに表示）、`plt.pause()` のループや `FuncAnimation` は実行中にライブで更新されます。matplotlib を使う Card には日本語フォント（IPAex）が入り、日本語のラベルもそのまま描けます。
- Plotly の `fig.show()` と Altair の `chart.show()` は、Card の出力表示に対話できるグラフとして出ます（拡大表示でズーム・ホバー・凡例の操作）。描画には同梱の plotly.js / Vega を使い、ネットワークには出ません（Card のグラフは外部からデータを読めません）。Plots デモを参照してください。
- 出力表示は Canvas 上の独立した枠として自由に配置・拡大でき、そのレイアウトは `layout.json` に保存されます。Card の名前を変えると位置・枠・グループは新しい名前に移り、Canvas から消すと一緒に消えます（UI でも Agent でも `tsugumid` の保存で揃います）。

## 8. UI（ワークベンチ）

- 左：階層・Card 定義・表示中の Card / 中央：Canvas / 下：状態バーとログ・タイムライン / 右：Agent チャット。
- Canvas：Card の追加・移動（グリッド吸着）・複製・配線・範囲選択・ズーム・整列（線がまっすぐになる層状の配置。Card・出力の枠・付箋が重ならず、1 回の取り消しで戻る）、グループ、Markdown の付箋、出力表示の枠。
- Card 設定ウインドウ：Nix 定義・ソース（構文強調・自動整形・エラー箇所の表示）・入出力・実行履歴・成果物。作業コピー上で編集し、「適用」で反映。
- 複数のプロジェクトを同時に開けます。アプリはウインドウごとに 1 つのプロジェクト、Web UI はブラウザのタブごとに 1 つです。編集・実行・チャットはプロジェクトごとに独立し、片方を閉じてももう片方は動き続けます（[ui-view.md](ui-view.md#複数のプロジェクト)）。
- プロジェクト専用の VM 内ターミナル（そのプロジェクトの名前空間と専用 uid で bash を起動）。
- 日本語 / English の表示切り替え。

画面仕様の詳細は [ui-view.md](ui-view.md) を参照してください。

## 9. Agent

`agent/` として分離した独立パッケージです。Agent は UI と同じ RPC を使う 1 つのクライアントで、Agent を使わなくても Card の作成・編集・実行はすべて画面から行えます。

### モデルとの接続

- Anthropic Messages API と OpenAI Responses API に対応し、ストリーミングで応答します。OpenAI は API キーのほか、利用者の ChatGPT プラン（Sign in with ChatGPT、オープンソースのツール向けの手順）でも使えます（[セキュリティ](security.md#chatgpt-プランsign-in-with-chatgpt)）。
- チャットのモデルメニューで effort と Fast mode を選べます。モデルごとに使える値は `agent/Shared/agent.json` の表にあり、各社の文書で確かめたモデルだけが選択肢に出ます。使えないものは灰色の行で理由を示し、リクエストにも載せません。Fast mode は料金の倍率を項目に書き、Anthropic は Claude Opus 5.5、OpenAI は API キーの接続で料金表にあるモデルだけで選べます。ChatGPT プランでは選べません。何も選ばなければ、これらの項目を送らず、モデルの既定で動きます。選んだ effort は対応するモデルすべてに、Fast mode は選んだモデルだけに効きます。変えたときは次のリクエストから効き、途中で変えるとプロンプトキャッシュは効かなくなります。Agent とその helper のリクエストに使い、ガイド、思考の過程、版の要約、文脈の圧縮には使いません。
- チャットのモデルメニューは、開くたびに設定済みの全接続のモデル一覧を取得し直します。保存済みの一覧をすぐに表示し、新しい一覧は届いた接続から反映します。失敗した接続や遅い接続があっても、保存済みの一覧でそのまま選べます。一覧の保存先は設定の「API」と同じです。

### tool と RPC

- Agent が使う tool の定義は `tsugumi.bfbs`（FlatBuffers スキーマの doc comment 付き）から自動生成し、tool 引数と FlatBuffers メッセージの相互変換も同じスキーマで行います。RPC 契約を更新すれば、Agent の tool も自動で追従します。
- 変更を伴う tool 呼び出しには Agent が一意な `op_id` を付け、UI とまったく同じリビジョン検査・競合検出・確定通知の経路を通ります。
- Mac のフォルダをチャットに添付すると、Agent が `AttachedFolderRead` で読めます（そのプロジェクトだけに読み取り専用で共有し、読んだ内容はモデルに送られます）。

### Web 検索

- 設定の「API」でオンにすると、チャットの Agent が自分の判断で Web を検索できます。既定はオフで、オフの間はモデルにツールを見せません。検索語が外に出ること、未公開のデータが載る危険は[セキュリティ](security.md#web-検索)にあります。
- 検索はプロバイダの機能（Anthropic の `web_search`、OpenAI の `web_search`。ChatGPT プランでも同じ）で行います。Tsugumi は検索サービスに接続しません。ローカル fixture には検索がありません。
- 検索はチャットにほかの tool と同じ行で出て、検索語と見つかった URL をたどれます。応答が引用した出典は応答の下にリンクで出ます。ChatGPT プランでは、引用された出典だけが出ます（`include` を送っていないため）。
- 使えるのはチャットの Agent だけで、helper とガイドのツグミは検索しません。

### Draft から Card を生成

- Draft の概要・処理内容・追加コンテキスト・ポートと前後の配線を Agent に渡し、型・Nix 定義・ソースコードを 1 トランザクションで保存させてから、ビルドと実行による検証まで行わせます。
- 順序は「先に書いて、あとで実行」です。Agent は Card をできるだけ書き終えてから、トリガーから 1 回で実行して確かめます。途中で実行するのは、次を書くのに実行でしか得られない情報が要るときだけです（ダウンロードしたファイルの列など）。
- 途中の失敗やキャンセルでは、中途半端な変更を保存しません（計画は残ります）。
- Draft に書かれていた意図はそのまま残ります。空だった概要と追加コンテキストは、チャットでの指示と Draft の内容から Agent が埋めます（書かれていないことは足しません）。埋めない保存と、書かれていた内容を変える保存は `tsugumid` が拒否します。

### コメントから Card を生成

Card に「こう変えたい」と書き、Agent に従わせます。Draft が実装前の Card の計画なら、コメントは Card そのものへの指示です。どの種類の Card にも付けられます。

- Card の右クリックの「コメント…」か、Canvas の Card の右上のバッジから書きます（Card 設定の「コメント」）。追加・編集・解決・開き直し・削除はその場で保存されます（実行中に書いたものは、ほかの編集と同じく実行が終わると保存されます）。
- Canvas の「コメントから生成」で、未解決のコメントをチャットの Agent に渡します。選択した Card があればその Card のコメントだけ、なければ表示中の階層の全部です。渡したコメントはチャットに出ます。
- Agent は既定でコメントが付いた Card を書き換え、コメントが求めれば Card を足したり分けたりします。保存は Draft と同じ道具で、Card の権限を広げる保存には同じ承認が要ります（[Agent の権限境界](security.md#agent-の権限境界)）。
- Agent も人間と同じようにコメントを書き、編集し、解決し、消せます。代わりに、誰が何をしたかが残ります。

**誰が何をしたか**

- Agent の保存では、`tsugumid` が保存の `origin` から記録を書きます（`core/src/comments.cc`）。モデルがこの欄に何を書いても使いません。
  - `author`: 書いた人。`editedBy`: 最後に本文を変えた人。`stateBy`: 最後に解決または開き直しをした人。
  - `cards`: そのコメントを解決した保存で変わった Card。何も変えずに解決したときは空です。
- Card 設定のコメントには、Agent が書いたものや変えたものに「Agent」の印が出て、解決の行に何をしたかと変わった Card が出ます。解決済みは消えずに畳まれ、誰でも開き直せます。
- チャットの「変更を保存」の行の下に、その保存がコメントをどう変えたかが出ます（保存の結果の `comments`）。
- 版の履歴の詳細に、その版でコメントがどう変わり（追加・編集・解決・開き直し・削除）、誰がして、どの Card が変わったかが出ます（[版の管理](versions.md#コメント)）。

**置き場所**

`definitions/<id>/_meta.json` の `comments` です。実行に関わらないので、結果は無効になりません。Card の名前を変えてもついていき、複製したときは複製にも付きます。

付箋との違い: 付箋は Canvas の好きな場所に置く人間向けのメモで、`layout.json` にあり、Agent に渡す流れはありません。コメントは Card に属し、状態と記録を持ち、版の要約の対象でもあります。

### 手分け（delegate）

- 互いに依存しない作業（線でつながっていない Draft など）は、Agent が helper の Agent に分けて同時に進められます。
- helper はそれぞれ自分の会話と同じツールを持ち、受け持った Card の定義だけを保存できます（`tsugumi.json`・配置・ほかの Card は変えられません）。
- helper は、受け持った Card の定義と `_meta.json`、そこにつながる線、上流で値を作る Card（型とコード）を、アプリが読んだものとして最初の発言で受け取ります。プロジェクト全体を読まずに始められるので、親の指示には Draft やルールに書いていないことだけを書きます。
- helper どうしの保存と実行がぶつかったとき（実行中の `busy`、`revision_conflict`）は、Agent の側で実行の終わりを待ち、最新のリビジョンから送り直します。
- 待ち合わせの戦術は、Agent が手分けするときに選びます（`delegate` の `runs`）。`each`（既定）では helper がそれぞれ実行し、1 つの実行が終わるまでほかの helper の保存と実行は待ちます。`together` では helper は書いて検証するだけで（実行しようとしても断られます）、全員の報告がそろってから Agent が 1 回で実行します。Agent は、helper が書くのに自分の実行の結果を必要とするときを除いて `together` を選びます。
- helper の報告がそろうと、Agent がそれぞれの Card を確かめて報告します。helper の会話は元の会話の隣（`<会話>.helpers/`）に残ります。

### Skill と会話

- Skill（プロジェクト内の `skills/` や、設定で追加したフォルダ）を `$` で呼び出せます。
- 会話の一覧・全文検索・要約による圧縮に対応しています。

### ツグミ（チャットとガイド）

プロジェクトのチャットはツグミです。

- 鳥はチャットの左上（チャットを閉じているときはチャットのボタンの上）で待ちます。画面のものを説明するときは `guide_show` でそこへ飛んでいき（▶ 実行、Card、デモのカードなど）、吹き出しで一言話します。
- 吹き出しには、タップできる返事（「次へ」「もっと詳しく」など）、押すと動くボタン（デモを開く・実行・停止・Card 設定を開く・API の設定・新規プロジェクトなど）、自由に返事できる入力欄が出ます。タップした返事は、チャットに利用者の発言として届きます。
- 吹き出しを閉じると元の場所へ戻ります。動きを減らす設定では、その場に現れます。⌥⌘G でチャット（Home では会話パネル）を開閉します。
- チャットのツグミが飛び回るのはデモの案内のときだけです。組み込みの Skill `$demo`（`agent/Shared/demo-skill.md`）をデモの案内が始めるか、チャットで `$demo` と書くと始まり、最後のあいさつ（「$demo でいつでもお呼びください…」）で終わります。それ以外はふつうのチャットです。
- 案内の中では、Card in Card の中（`card:<外側>/<内側>`。Canvas がその中を開きます）や、ツグミが開く Card 設定のウインドウ（`open`、`param:<名前>`）にも飛んでいきます。プロジェクトを読み込む間は、読み込み中のカードの鳥に重なるように飛び込んで大きさを合わせ、読み込みが終わるとそこから出てきます。

Home では、アプリ全体で 1 つのガイドの会話（`Agent.Role.guide`、規則は `agent/Shared/guide-prompt.md`）が右下から同じように話します。

- ガイドが持つのは、読み取りの tool と `reference_read` と `guide_show` だけです。保存・実行・停止はしません。
- 会話は、Mac では `~/Library/Application Support/Tsugumi/agent/guide/`、Web ではデータフォルダの `agent/guide/` に残ります。
- 初回起動では、Anthropic か OpenAI を選んでどちらか一方の API キーを入力し、接続を確かめてから始めます（「スキップして始める」も選べます）。キーは設定の「API」と同じ経路で Keychain に保存します。すでに使えるキー（または ChatGPT のプラン）があれば何も聞きません。キーのない間は鳥も会話も出さず、キーを登録すると現れ、削除すると消えます。
- 初めて現れたときは「何に興味がありますか？」と尋ねます（入力するか、候補をタップ）。答えに合うデモを説明とタグから選び、デモ一覧を開いてそのカードへ飛び、理由を一言添えて「このデモを開く」を出します。デモを開くと、そのコピーのチャットのツグミが案内を続けます。
- 案内中は、実行の開始・終了（成功・失敗・停止）と接続の切断・再接続を、実際の状態のままチャットに伝えます。答えている間に起きたことは、答え終わってから伝えます。案内の状態と勧誘への回答は保存され、再起動しても繰り返しません。

**確かめる場所**

- tool の一覧と FlatJSON の変換が Mac と Web で同じこと: [test_agent_contract.py](https://github.com/gen740/Tsugumi/blob/main/tests/python/test_agent_contract.py)、`macOS/CoreTests`
- Draft を実装する保存の規則: [test_draft_intent.py](https://github.com/gen740/Tsugumi/blob/main/tests/python/test_draft_intent.py)
- コメントの保存の規則と版の履歴: [test_card_comments.py](https://github.com/gen740/Tsugumi/blob/main/tests/python/test_card_comments.py)
- Agent が Card の権限を広げる保存には承認が要ること: [test_permissions.py](https://github.com/gen740/Tsugumi/blob/main/tests/python/test_permissions.py)
- 実際の Claude API で、Draft から Card を作って実行・検証した記録: [AgentRun.md](agentrun.md)

## 10. デモ

同梱のデモは 31 個あります（Draft 版を含む）。何を試せるかの一覧は [README](../index.md#使ってみる)、各デモの Card のつながりと動きは[同梱デモのしくみ](demos.md)にあります。
デモはホームの「デモを見る」から「コピーを開く」を選ぶと、`~/Documents/Tsugumi/<demo>` に編集できるコピーができます。

**確かめる場所**

- すべてのデモが開けて解決できること: [test_demos.py](https://github.com/gen740/Tsugumi/blob/main/tests/python/test_demos.py)
- デモを実際に動かして、独立に計算した値と照合すること（`TSUGUMI_TEST_DEMO_RUNS=1`）: `tests/python/test_demo_*.py`
