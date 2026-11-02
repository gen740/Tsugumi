# 実装依頼: Tsugumiの試作

Tsugumiは、Cardをポートで接続し、Nixで固定した環境で処理を実行・記録するmacOSアプリです。条件分岐・合流・有限のfeedback loop・入れ子のグラフを扱います。通常の計算にLLMは必要ありません。

SwiftUIのUIと、Appleの`container`による隔離実行環境を含む、実際に動く試作を作ってください。この文書だけで実装依頼が完結します。記載するパスは作成先であり、別の資料や既存コードの提供は前提にしません。

## 1. 構成と実装方針

- macOSホスト: SwiftUI UI、Dawn描画層、未確定編集、Undo/Redo、UIに接続するAgent、containerの起動・接続管理。
- コンテナ内: `tsugumid`、C++ core、Nix、Card worker、保存・実行履歴。
- UI・Agentとcore: 同じFlatBuffers RPC契約を使う独立したクライアント。coreの内部関数・DB・文書ファイルを直接操作しない。
- ビルド: C++はCMake/Ninja、SwiftはSwiftPM、ツール・依存の固定はNix。CMakeがC++のソース一覧・リンク・生成依存を所有する。
- binding: 標準`flatc`でC++・Swift・Python用を生成する。生成物はGitへ登録しない。

`tsugumid`はプロジェクトごとに一つのプロセスとし、文書管理・RPC・実行を所有させます。別のhostサービスや独立した実行CLIは作りません。検証用プログラムもRPCクライアントとします。派生状態の二重保持、独自のbinding生成器、将来のためだけの抽象層は不要です。

第一の試作はApple silicon Mac上のアプリを対象とします。Web UIは将来追加する予定ですが、今回は実装しません。別OS向けの配布や複数のコンテナ基盤への対応も追加しません。採用するApple container版・対応macOS・コンテナイメージを固定してください。ストア配布・公証は対象外ですが、ローカルで起動できるmacOSアプリは納品対象です。

### core・UI・Agentの境界

```text
コンテナ                         macOSクライアント
core (tsugumid) ←── RPC ──→ UI (SwiftUI + Dawn)
       ↑                         └── Agent（対話・モデル接続）
       └──────── RPC ────────────────┘
```

coreはUI・描画・モデルproviderに依存せず、UIを起動しなくてもRPCから保存・実行・履歴取得ができます。クライアント間で共通にするのは`protocol/`の通信契約です。macOSのUIとAgentはSwiftのRPCクライアントを利用し、将来のWeb UIは同じFBSからTypeScript bindingを生成して別のクライアントを実装します。いずれもcoreの内部ヘッダーをリンクしません。UIを変更しても実行契約が変わらず、Agentを無効にしても通常操作が成立する構造にしてください。

UIはAgentの会話・進捗と、明示的に選んだCardなどの文脈を扱います。Agentによる文書操作はcoreへ直接RPCで送り、UI操作の模倣やUI内部状態の書換えには依存しません。両者の変更には同じrevision・競合検査・確定通知を適用します。

### ソース構成

```text
tsugumi/
├── CMakeLists.txt
├── flake.nix / flake.lock
├── README.md
├── macOS/
│   ├── Package.swift
│   ├── Sources/                  # SwiftUI、編集状態、container起動管理
│   │   └── RPC/                  # macOS用の接続・要求・返信・通知処理
│   ├── Resources/
│   ├── Container/               # Apple container用の最小イメージ・起動・永続化設定
│   └── Tests/
├── ui/renderer/                  # C++のDawn描画層。coreから独立
├── core/
│   ├── include/tsugumi/core/      # 公開API
│   └── src/                      # tsugumid、文書・実行・履歴
├── protocol/
│   ├── tsugumi.fbs               # 外部RPC
│   ├── worker.fbs                # worker呼び出し
│   ├── data.fbs                  # 共通の値
│   └── tsugumi_enums.fbs
├── sdk/                          # C++・Python SDK、worker ABIと実装
├── agent/                        # UI側のモデル接続・RPC toolアダプター
├── tests/
│   ├── cpp/
│   ├── python/                   # 永続化・worker・RPC統合テスト
│   └── fixtures/                 # ローカルモデルproviderなど
└── demos/                        # UIから開く編集可能なデモと一覧定義
```

Apple container固有のイメージ定義と配備設定は`macOS/Container/`に置き、起動管理のSwiftコードは`macOS/Sources/`に置きます。coreのビルド定義はルートのCMake・Nixで管理し、イメージ作成側ではその成果物を利用します。

RPCクライアントの実装は各UI側に置き、ルートに共通の`client/`は作りません。生成bindingとビルド成果物は各ビルドシステムの出力先に置き、ソースから分離してGit対象外にします。将来の`web/`は同じ通信契約でcoreへ接続し、macOSの実装やSwiftUIの状態を経由しません。

各ライブラリの公開APIは`include/tsugumi/<component>/`、内部実装は`src/`に置きます。製品名はTsugumi、実行環境名は`tsugumid`とし、ファイル・名前空間・SDK・環境変数もこの命名に統一してください。

## 2. SwiftUIと隔離環境

### UI

プロジェクト一覧、Cardとポート接続のキャンバス、Card設定・ソース編集、診断、実行・取消、履歴・ログ・成果物、Agent操作を提供してください。キャンバスは追加・削除・移動・接続・選択・パン・ズーム・入れ子グラフへの移動に対応し、Display通知の表示データも描画します。

未確定編集とUndo/RedoはUIが管理し、確定には`ProjectSaveRequest`を使います。保存済みの変更を戻す場合も新しい保存として確定します。競合時は編集を保持して診断を表示してください。配置・選択・カメラは保存して復元します。

起動中・準備完了・停止・失敗・再接続をUIに表示します。RPCやコンテナの準備待ちで画面を固めず、失敗時は原因と復旧手順を示してください。

### Dawn描画層

SwiftUIとネイティブUIはウィンドウ・設定・一覧・チャット・Card・入力欄・文字編集・選択とフォーカスを担当します。Dawnの担当はDisplayのGPU描画に絞り、大量の接続線などは必要性を測ってから追加します。macOSではMetal backendと描画surfaceをSwiftUIに組み込み、C++描画層との接続は薄いbridgeに限定します。Dawnの版を固定し、描画層もCMakeでビルドしてください。

coreは型付きの表示データを送り、UI側が描画用のscene・GPU資源へ変換します。GPU handle、SwiftUI型、Dawn型をRPCや保存文書へ持ち込まず、coreに描画依存を追加しません。Dawn・shader・描画用資源はホスト側に置き、最小コンテナには含めません。

第一の試作ではLive Streamの折れ線をDawnで表示し、サイズ変更・高DPI・更新の集約・資源解放を確認します。文字入力・フォーカス・アクセシビリティをDawn側で再実装しません。後で表示種別を増やせる境界を作りますが、汎用プラグイン基盤・3D・WASM実行系は先行実装しません。

### Apple container

Appleの`container`が提供する軽量Linux VMを隔離基盤とします。プロジェクトごとに環境を分け、同じプロジェクトの複数ウィンドウは同じ`tsugumid`へ接続します。ホスト側は起動・接続・停止だけを管理し、Cardコードやshellは実行しません。

- ホストとの共有は、選択したプロジェクトと必要な専用永続領域に限定する。ホーム全体、SSH鍵、ホストのNix store、container管理socketは渡さない。
- ホストとコンテナのパスを対応づけ、外部プロジェクトの取り込みにも同じ許可範囲を適用する。
- コンテナ側のNix store・キャッシュを永続化する。コンテナ再作成で文書・履歴・成果物を失わず、停止とデータ削除を別操作にする。
- RPCを不用意に外部公開しない。コンテナ内のビルド取得・Card実行と、ホスト側Agentのモデル接続の通信許可を区別し、CPU・メモリ・ディスクに上限を設ける。
- ホストからの隔離と、コンテナ内のCard間のアクセス制限を区別する。Cardへcoreの資格情報や環境全体を継承させない。
- `sandbox: false`でもホスト実行や追加のホスト領域公開は許可しない。指定した制限を満たせない場合は拒否し、制限解除やホスト実行へ自動で切り替えない。
- UI切断だけではrunを止めない。明示的な環境停止と異常終了では、進行中runの中断と復旧を扱う。

### 最小構成と評価指標

コンテナはこのアプリの実行に必要な最小構成にします。Nixで必要な依存だけを閉包として組み立て、各依存の用途を説明できるようにしてください。

- 基本イメージは`tsugumid`・Nix・起動と通信に必要な実行時依存だけにする。汎用開発環境、デスクトップ、SSHサーバー、Docker、不要な常駐サービスは含めない。
- Cardのコンパイラー・Python・個別ライブラリは固定したNix依存として必要時に取得し、永続storeで再利用する。ホストの開発環境には依存しない。
- ビルド用依存・実行用依存・キャッシュを分け、コンテナごとに同じイメージを複製しない。初回準備と通常起動を区別してUIに進捗を出す。
- 初期実行枠は2 vCPU・メモリ4 GiB、永続領域の容量上限は20 GiBを試作の目安とする。必要量を測って調整可能にし、上限を適用できない項目を黙って保証しない。

以下は試作の評価目標です。使用Mac・設定・デモ・測定回数を記録し、未達なら実測値と支配的な要因を報告します。数値を満たすために隔離・保存・実計算を省略しないでください。

| 指標 | 測定・目標 |
| --- | --- |
| 容量 | 基本イメージの圧縮／展開サイズ、Nix依存取得後のstore、履歴・成果物を分けて実測。大きい依存の用途を説明 |
| 起動 | 初回取得・ビルドと、キャッシュ済みのアプリ起動→デモ操作可能までを別計測。後者は10秒以内を目標 |
| UI応答 | デモ実行中も入力・選択・ドラッグへ100ms以内に反応。処理中の表示で入力を塞がない |
| 取消 | クリックへの反応は100ms以内、通常のデモworker停止は2秒以内を目標。Nix準備中の取消は別計測 |
| 継続利用 | メモリ・store・履歴の増加量を記録。Live Streamを10回実行しても終了済みworkerや購読が蓄積しない |

## 3. プロジェクトと保存

```text
project/
├── tsugumi.json                  # 名前、Nixpkgs、DB接続、Cardと接続
├── layout.json                   # 配置・付箋・表示接続
├── definitions/
│   └── <definition-id>/
│       ├── _card.json            # Cardの既定値・params・入力の受け口
│       ├── _outputs.fbs          # 出力ポートと型の正本
│       ├── default.nix           # ビルド・実行段階
│       ├── src/
│       └── _meta.json            # 任意。Draftの説明など
├── skills/                       # 任意。Agent用
└── .tsugumi/                     # Git対象外
    ├── view.json                 # カメラ・選択など
    ├── document.journal          # 保存・receipt・クラッシュ復旧
    ├── history.sqlite            # 履歴・ログ・status・最後の入力
    └── runs/<run-id>/            # 実行時ファイル・保持した成果物
```

`tsugumi.json`は`name`、`pkgsPath`、`databaseConnections: [{name, url}]`、`canvas.cards`、`canvas.connections`を持ちます。接続は`{from:{card,port}, to:{card,port}}`です。Cardと定義のIDは安定・一意とし、入れ子のscopeとIDをcore内で完全pathへ正規化します。

確定文書とrevisionの正本はcoreの文書セッションです。接続ごとに複製しません。型・診断・解決済みCardは文書から導出し、実行状態やsource hashを文書へ書き戻さないでください。

### プロジェクトの所有権と二重起動防止

複数のUI・Agentは一つの`tsugumid`へ接続し、文書ファイル・journal・履歴の書込みはcoreへ集約します。socketは通信路であり、接続しただけではファイルの排他制御になりません。

- 起動管理はホスト上でプロジェクトの実体を識別し、パスの別名でも同じ実行環境へ接続する。プロジェクト単位のOSファイルロックで環境の検索・起動・停止を直列化し、起動途中の重複も防ぐ。
- coreも専用の固定lockファイルに非待機の排他ロックを取得してから文書を開き、プロセスの寿命中保持する。取得できなければ二重起動として拒否する。保存対象のファイル自体は置換されるため、所有権ロックの対象にしない。
- PIDやsocketファイルの存在だけを生存判定にしない。異常終了後は実行環境と接続先の所有者を確認して復旧する。ロック取得前にlockファイルや他プロセスのsocketを削除しない。
- コンテナ内のロックが別VMやホストにも効くとは仮定しない。同じプロジェクトを複数環境へ書込み可能で割り当てない。安全な排他を確認できない保存先は拒否する。

OSファイルロックの用途は二重起動・所有権の競合防止に限定します。通常の編集はcoreのプロジェクトtransactionとして直列化し、クライアントへファイルごとのロック操作を公開しません。このロックは協調するアプリ間の所有権を守るものです。外部エディターの書込みまで防げるとは扱わず、外部変更はhash・revision検査と明示的なReloadで扱います。

### 保存契約

編集は`ProjectSaveRequest`によるプロジェクト単位のtransactionに集約します。引数は`base_rev`、`op_id`、`origin`、`changes[]`。対象プロジェクトは接続先で決まります。changesはグラフ・設定・配置・定義の変更とソースの追加・置換・UTF-8 byte patch・削除をまとめて表現し、既存ソースへの変更には`base_hash`を付けます。UIとAgentは同じ変更契約を使い、coreが検証・保存先への変換・確定・通知を所有します。保存ファイルへの分割や反映順序をクライアントに組み立てさせません。

1. 変更を文書モデルへ適用してID・接続・パス・基準revision・hashを検証し、transaction全体を確定できることを確認する。
2. 確定直前にも競合を検査し、journalを含めて全件成功または全件未適用で永続化する。
3. 成功後にrevisionを進め、同じ確定差分を返信と`DocPatched`で伝える。

同じ`op_id`・内容の再送には保存済みreceiptを返し、別内容での再使用は拒否します。同じrevisionから競合する保存は一方だけ成功させます。障害注入で部分更新が残らないことを確認してください。

未完成のCardや型定義も診断付きで保存でき、実行可能性はValidateとRunで検査します。文書revisionと実行revisionを分け、配置変更だけでは計算定義を再解決しません。外部変更はReloadで明示的に取り込み、未知の変更を自動rebaseしないでください。

Importは定義・ソースをコピーしてID衝突を解消し、配置は別の保存で行います。複製はクライアントの起動管理がRPCから取得した同じ版の文書とソースを新しいプロジェクトに組み立てます。未確定編集を混ぜず、キャッシュ削除で文書・履歴を消さないでください。

## 4. Cardと実行モデル

### 設定

定義の既定値は`_card.json`、インスタンスの上書きは`tsugumi.json`に置きます。省略と明示的なfalse・0・空文字・nullを区別し、上書きを削除した場合だけ継承へ戻します。

| 項目 | 契約・既定値 |
| --- | --- |
| `id` / `definition` | インスタンスID／定義ID。実装済みCardは定義を指定 |
| `name` / `symbol` / `description` | 表示用文字列。既定は空 |
| `language` / `template` | 定義のみ。言語／組み込み種別。既定はnone |
| `params` | 定義は`{name,type,data,nix}`の配列、インスタンスは名前と値のmap |
| `inputs` | 定義のみ。`{name,optional}`の配列。型は接続から導出 |
| `inputMode` | 定義のみ。all／any、既定all |
| `maxRuns` | 正の整数、既定1。0は拒否 |
| `timeoutSeconds` / `buildTimeoutSeconds` | 正の整数またはnull。省略は継承、nullは期限解除、既定は無期限 |
| `recordIO` / `retainArtifact` | 独立したbool、既定false |
| `artifactOutput` | 保持する出力ポート。retainArtifact有効時は実在必須 |
| `filesystem` / `sandbox` | 作業・出力領域の要求／ファイルアクセス制限。既定false／true |
| `loggerDatabaseConnection` | 任意のDB接続名。指定時は存在を検証 |
| `canvasEnabled` | Display公開、既定false |
| `project` / `canvas` / `boundary` | インスタンスのみ。外部グラフ／内包グラフ／入出力Endpoint map |

paramsの型はbool・int64・uint64・double・stringと各配列型です。型と数値範囲を検証し、暗黙変換や整数の桁落ちを避けます。全実効値をworkerへ渡し、`nix: true`の値だけをNix評価へ渡します。typeとnixは定義から継承します。

### 型とポート

ポート種別はFile・Directory・FlatBufferです。通常Cardの出力型は`_outputs.fbs`を正本とし、入力型は接続元から導出してSDK用の`_inputs.fbs`とBFBSを生成します。手書きの入力型や解決済みoutputsを別の正本にしないでください。

出力FBSのルートはポート一覧のcatalogです。たとえば`Outputs { samples:Samples (required); }`なら、samplesポートは独立したSamplesメッセージを運びます。catalog全体を入力bufferとして渡しません。必須出力の欠落は失敗、省略可能な出力の欠落は非発火です。

File／Directoryは文字列valueを持つメッセージにcatalog側で`tsugumi:"file"`／`tsugumi:"directory"`属性を付け、実体の存在・種類・許可範囲を検査します。型互換性を独自hashやBFBSバイト列の一致だけで判定せず、未解決の循環や不正な条件型には対象付き診断を返してください。

### 実行受付とスケジューラー

Runは確定文書の不変snapshotを使います。`base_rev`・対象Cardの完全path・入力・型を検証し、処理開始前にrun_idを割り当てます。同一文書では同時に一つのrunを受け付け、実行中の文書変更は拒否します。RunPlanは計画だけを返します。

`propagate: true`は対象から下流へ実行し、全体実行はStart Cardを対象にします。falseは最後に保存した入力で一枚だけ実行し、入力不足なら失敗します。別runの値を寄せ集めません。

- 入力ごとに到着順の有界queueを持つ。allは接続済みの各入力から一つずつ、anyは最も早い入力一つだけを消費する。
- 未接続の必須入力は拒否し、未接続の省略可能入力は待たない。上流の出力省略を空トークンや前回値で補わない。
- Start・明示的なsourceは開始時に一度だけ発火する。同一Cardは直列、別Cardは並列上限1〜64、既定4で実行する。
- 下流queueが満杯なら上流の受付を抑える。maxRuns超過は失敗とし、有限反復を扱う。
- 実行可能・進行中の処理がなくなれば収束させ、非選択分岐のskippedと、失敗・入力不足のblockedを区別する。

| 組み込みCard | 動作 |
| --- | --- |
| trigger | 空メッセージで開始。グラフごとに最大1個 |
| input | 設定値や選択したFileを出力 |
| fork | payloadを複数出力へ配る。不変bufferは共有可 |
| branch | bool条件に従いvalueをaかbだけへ出力 |
| merge | 到着入力を入力ポート名付きunionへ包む。anyで実行 |
| logger | 記録・表示。外部DB保存はcore履歴と独立 |
| subgraph | 内包または外部グラフを`boundary.inputs/outputs`で接続 |
| draft | 説明・計画を保持。未実装のまま実行しない |

fork・branch・mergeの出力は入力型と組み込み規則から導出します。mergeは同型でも到着元を保持し、型が無限に拡張する循環は拒否してください。

### Nixとworker

`default.nix`の入口は`{ pkgs, inputs, params }:`、SDKは`inputs.tsugumi`から取得します。設定はsdk、package、preScript、script、postScript、reuseWorker（既定false）です。packageは`lib/main.so`を含み、shellだけのCardでは省略できます。

入力が揃ったCardについて、入力契約・SDKを生成し、Nix評価・ビルド、入力検証、`preScript → main.so → script → postScript`、出力検証、永続化・通知・下流転送の順に進めます。

Nix評価とGCは専用レーンに集約し、pure/restricted・IFD禁止を基本にします。Cardの固定ソース・Nixpkgs・SDK・明示した上流参照だけを渡し、非選択分岐はビルドしません。成果物を再利用しても実行と入出力検証は毎回行います。

C++とPythonは共通のmain.so ABIを使い、Python packageはinterpreterと依存を含めます。別プロセスのworkerへ型付き入力・params・許可した環境・作業先を渡し、ポート名付き出力bytesを受け取ります。reuseWorkerは同じrun内かつ同じ設定・権限に限ります。

shellには`JSON_<PORT>`、必要に応じて`INPUT_<PORT>`・`FILE_<PORT>`・`DIR_<PORT>`を渡します。出力は`$TSUGUMI_OUTPUT`のJSONをポート別FBSへ変換し、stdout/stderrはログにします。preScriptの環境と作業先を引き継ぎ、前段の失敗を後処理の成功で消さないでください。

### 終了と取消

runの最終結果は成功・失敗・取消として一度だけ確定します。RunCancelは冪等に新規呼び出しを止め、workerと子プロセスを取り消し、必要な保存後に完了します。取消の受付と完了は区別してください。

Cardの期限は準備後のpreScriptからpostScriptまで、Nixの期限は処理枠取得後から計測します。再接続で状態を取得でき、core再起動時の未完了runは中断として扱います。自動再実行やworkerメモリの復元はしません。

## 5. 履歴と障害処理

SQLiteをrun・呼び出し・進行status・ログ・最後の保存入力の正本とします。呼び出しはrun_id・完全Card path・呼び出し番号で識別し、実行revision・source hash・時刻・結果・所要時間・Failureを記録します。存在しない時刻やrevisionをゼロで補いません。

履歴の書き込みは有界queueを使い、呼び出し完了とそのstatus削除を同じtransactionで行います。古い完了が新しいstatusを消さないようにし、永続化後に通知します。最新呼び出し・record・集約・status・run情報の読取も一つのtransactionにまとめます。

`HistoryState`はcoreが選択します。過去の呼び出しに別呼び出しの状態を混ぜず、最新追跡では対応するstatus・残存する集約失敗・run取消を反映します。時刻は対応statusか記録済み終了時刻から取得し、根拠がなければ省略します。

ログはstdout/stderrと段階別の構造化journalを持ち、全体用・呼び出し用に二重保存しません。UTF-8 byte cursor、範囲・末尾取得、フィルター、ページング、省略情報に対応し、分割による欠損・重複を防ぎます。

recordIO無効でも結果・失敗・ログ・最後の入力を保持します。全呼び出しのI/Oがあるようには返しません。成果物保持は独立し、実体をファイル、参照をSQLiteに保存します。読取ではexists・recording_disabled・truncated・orphanedを区別し、外部Logger DBを必須にしません。

Failureは`kind`・名前付き`args`・任意の`diagnostic`を持ち、競合・パス・型・入力不足・回数超過・期限・取消・Nix・worker・出力検証・永続化・通信破損を区別します。翻訳文で種別を判定しません。worker crash・破損buffer・必須出力欠落を成功にせず、不正出力を下流へ流さないでください。

## 6. RPC契約

この節を新規FBSの実装契約とします。名前だけの一覧から挙動を推測せず、以下の型・既定値・成功応答・副作用をスキーマとテストに反映してください。Agentのモデル処理はcore RPCに含めません。

### 6.1 境界・表記・共通型

一つの接続先は一つのプロジェクトだけを所有します。初期化済みのプロジェクトルートは起動時に渡し、接続後の要求にホスト絶対パスや別プロジェクトの`file`を渡しません。Helloで返す`project_id`はルートの保存形式を増やさず、内部状態に永続化したUUIDです。再起動で維持し、複製時は新規にします。

Homeの一覧・作成・デモのコピー・実行環境の起動はクライアント側の起動管理が担当します。`ProjectList/Create/CopyRequest`はcore RPCに設けません。新規作成は起動前に初期文書を配置し、以後の編集はRPC経由とします。複製はInspectの文書・ソースmanifestを基にReadで同じ版の内容を取得し、全取得後にrevisionを再確認して新しい保存先へ組み立てます。履歴・cache・未確定編集はコピーしません。

以下は型の略記です。`?`は省略可、`=値`は省略時の既定値、それ以外は必須です。空文字や0を「未指定」の代用にしません。FBSの必須scalarはoptional scalarとして存在を検証し、文字列・table・vectorはrequiredまたは意味検証で保証します。規定した名称は新仕様の名前であり、既存のtag番号や型名との互換性は要求しません。

| 型 | 表現・制約 |
| --- | --- |
| `Id` | UUID文字列。project_id・op_id・run_id・client_id・view_idに使用 |
| `Rev` / `Seq` | uint64。revisionは初期0、確定ごとに増加。sequenceの有効値は1以上 |
| `CardPath` | IDの配列。例`["nested","step"]`。空はグラフrootだけに使用、Card指定には不可 |
| `Hash` | SHA-256の小文字hex。ファイルは実bytes、文書はcoreが返すcanonical JSONのUTF-8 bytesを対象 |
| `Path` | プロジェクト相対のUTF-8パス。絶対パス・`..`・NUL・許可範囲外へ出るリンクを拒否。rootの一覧は`.` |
| `Time` / `Duration` | UTC Unix epochのint64ミリ秒／uint64ミリ秒。未知の時刻は省略 |
| `Page` | `cursor:string?`, `limit:uint32=100`。limitは1〜500 |
| `Mutation` | `base_rev:Rev`, `op_id:Id`, `origin:string`。originは表示・追跡用で認証には使わない |
| `Ack` | 空table。対象の処理が完了した場合だけ返す |
| `Diagnostic` | `severity:error\|warning`, `code:string`, `message:string`, `card:CardPath?`, `path:Path?`, `offset:uint64?` |
| `Failure` | `kind:FailureKind`, `args:[{key,value:string}]`, `diagnostic:string?`, `current_rev:Rev?`, `retry_after_ms:uint32?` |

JSONはUTF-8、重複キー・NaN・Infinityを拒否します。文書のcanonical化はcoreの一実装が所有し、snapshotとDeltaにその表現を返します。クライアントは文書hashを独自に再計算せず、ソースhashは受信したraw bytesから検証できます。JSONの整数は精度を保って解析し、Agent toolの64 bit引数は10進文字列からFBS整数へ変換します。文書JSONは元の整数表現を保持して転送します。FBSのmap表現はkey/valueのvectorで、重複keyを拒否します。CardPathは`{segments:[string]}`のtableとして表現し、IDに`/`を許可せず、文字列pathとの混同を避けます。

### 6.2 接続・フレーム・多重化

`Tsugumi.RPC.Message`をrootとし、file identifierは`TSG1`、通信世代は1、WebSocket subprotocolは`tsugumi.rpc.v1`に固定します。世代が違えば拒否し、暗黙変換しません。tableのfield ID・union tagを明示し、同じ世代で変更・再利用しないでください。必須unionにはNONEを認めず、未知enumも意味検証で拒否します。

```text
Message { request_id:uint64?, payload: Request | Reply | Notification }
Request { value: RequestPayload }
Reply   { value: 各要求の成功型 | Failure }
Notification { subscription_id:Id, seq:Seq, value: NotificationPayload }
```

要求は接続内で重複しない1以上のrequest_idを必須とし、返信は同じIDを使います。通知のrequest_idは省略し、op_id・run_idで因果関係を表します。受理した要求には成功型またはFailureを一度だけ返します。接続切断後の配送は保証せず、操作の確定と返信の到着を区別します。未知の返信ID・型の不一致はクライアント側でも検出します。

同じOS内ではUnix stream socketに4 byte little-endian本文長とFlatBufferを流します。VMをまたぐUI・Agentは`/rpc`のWebSocketを使い、1 binary messageを1 FlatBufferとします。WebSocketでは長さprefixを重ねず、fragmentを組み立てた後に検証します。共有フォルダ上のUnix socketでVM境界を越えられるとは仮定しません。

起動管理に`server.ready {endpoint, token, protocol, project_id}`を返します。WebSocketは許可Origin・subprotocolを検証し、最初のHelloでtokenを照合します。ブラウザ以外のOrigin省略は起動設定で明示的に許可したクライアントに限ります。tokenをURLやログに含めず、遠隔公開をする場合は暗号化transportを必須にします。Unix socketは親0700・本体0600と同一ユーザーの認証を使います。

Hello成功前は他の要求を実行しません。Helloは接続後5秒以内に要求し、同一接続での再Helloはinvalid_requestで拒否します。server_instance_idはdaemon起動ごとに新規発行します。

接続数は128、接続ごとの未完了要求は64、変更queueは128件、待機期限は30秒、送信待ちは256 messageまたは32 MiBを初期上限とします。本文は最大16 MiB。上限超過・検証不能なframeは当該接続を閉じ、IDを安全に取得できる意味エラーにはFailureを返します。未知のunionや不正長は接続エラーです。遅いpeerを切断しても、受理済み操作や他peerを止めません。

single processでも全要求を同期実行しません。文書変更とRun受付のrevision検査は同じ直列レーンを使い、Nix・worker・永続化I/Oの待機で通信loopを塞がないでください。読取は確定snapshot／DB transactionを使い、取消・状態取得・通知は長い処理の完了を待ちません。満杯は`busy`、待機期限超過は`timeout`で返し、その要求を後から実行しません。

### 6.3 snapshot・変更・再送

`ProjectSnapshot`は次の情報を持ちます。ソース本文やGPU資源は含めません。

```text
ProjectSnapshot {
  project_id:Id, rev:Rev, execution_rev:Rev,
  project_json:string, layout_json:string,
  definitions:[{id:string, card_json:string, meta_json:string?}],
  sources:[SourceInfo],
  resolution:[ResolvedCard], diagnostics:[Diagnostic],
  run:RunStatus, view:ViewState?, view_id:Id?, view_rev:Rev?
}
ResolvedCard {
  card:CardPath, inputs:[Port], outputs:[Port], diagnostics:[Diagnostic]
}
Port { name:string, kind:File|Directory|FlatBuffer, optional:bool,
       schema_bfbs:bytes?, root_type:string?, path:Path? }
```

`Definition`はsnapshotのdefinitions要素、`SourceInfo`は後述のFileStat応答です。snapshotとDeltaのsourcesはkind=file・exists=trueのSourceInfoを使います。

resolutionは導出結果です。解決済みPortは全kindでschema_bfbsとroot_typeを必須とします。解決できない型はschemaを省略して診断を付け、保存失敗とは区別します。初版は文書JSON合計8 MiB、source manifestは10,000項目を上限とし、snapshotと保存Deltaの全体が16 MiBに収まることも保存前に検証します。上限超過は`resource_limit`として拒否し、snapshotを黙って切り詰めません。source本文は分割取得します。

`ProjectSaveRequest`の`changes:[DocumentChange]`を、次のunionとして定義します。

| variant | 必須内容・意味 |
| --- | --- |
| `SetProject` | `json:string`。tsugumi.jsonに相当する文書モデル全体を置換 |
| `SetLayout` | `json:string`。配置文書全体を置換 |
| `PutDefinition` | `id:string, card_json:string, meta_json:string?`。定義作成／置換。meta省略は削除 |
| `RemoveDefinition` | `id:string`。定義と配下ソースを削除。最終文書に参照が残るなら拒否 |
| `PutSource` | `path:Path, bytes:bytes, base_hash:Hash?`。既存はhash必須、新規はhash省略かつ未存在が条件 |
| `PatchSource` | `path:Path, base_hash:Hash, patches:[{offset:uint64, remove:uint64, text:string}]`。UTF-8ソースの部分置換 |
| `RemoveSource` | `path:Path, base_hash:Hash`。既存ソースを削除 |

同じ対象への複数変更、定義削除とその配下編集の併用、空changesは拒否します。全変更を仮適用した最終文書を検査するため、定義作成・Card追加・接続追加は一transactionにできます。PatchSourceのoffsetは全て変更前bytesを基準とし、昇順・非重複・UTF-8境界を必須とします。Source操作でtsugumi.json・layout.json・_card.json・_meta.json・`.tsugumi/`やlockファイルを変更できません。保存形式への変換はcoreが担当します。

```text
DocumentDelta {
  base_rev:Rev, rev:Rev, execution_rev:Rev, op_id:Id, origin:string,
  project_json:string?, layout_json:string?,
  definitions_upsert:[Definition], definitions_removed:[string],
  sources_upsert:[SourceInfo], sources_removed:[Path],
  resolution:[ResolvedCard], diagnostics:[Diagnostic]
}
SaveResult { delta:DocumentDelta?, rev:Rev, execution_rev:Rev, replayed:bool }
```

Deltaの文書は変更部分の**置換後の全体**、definitionsとsourcesは変更した項目だけ、resolutionとdiagnosticsは置換後の全一覧です。クライアントはbase_rev一致時だけ適用し、rev適用済みなら重複を無視します。保存返信と通知には同じDeltaを使います。実効変更なしのSaveもreceiptは保存しますが、deltaなし・revision不変としDocPatchedを発行しません。ソース本文は通知せずhashを更新し、必要時にReadします。

Save・Reload・Import・Runはop_idを必須とします。要求の種類と全引数（request_idを除く）をdecodeして保存・比較し、同一op_id・同一要求は再実行せず元の結果を返します。別内容は`operation_conflict`です。再送判定はbase_revや現在のbusy状態の検査より先に行います。実行待ちの同一op_idも一つの処理にまとめます。既定値は展開して比較しますが、JSON文字列・ソースbytes・vector順序は要求に含まれた通りに比較します。再送は元の要求を保持して使い、同じop_idでJSONの整形や項目順を変えません。

保存のreceiptは文書確定と同時、Runのreceiptとrun_idは処理開始前に耐久化します。再起動後も保持し、初版ではreceiptを自動削除しません。op_idと要求を検証してqueueへ受理した操作はpendingとして照会でき、確定前の失敗も保存可能ならterminal receiptに残します。永続化障害で確定状態を判定できない場合は変更受付を止め、journal回復後にreceiptと実状態を照合してから再開します。新たな条件でやり直す場合は新op_idを使います。状態確認には`OperationGetRequest`を使い、接続切断やクライアント側timeoutを未実行の証拠にしません。Run再送が返すのは元の受付結果で、現在状態はRunStatusから取得します。OperationReceiptは`{op_id:Id, result:SaveResult|ImportResult|RunAccepted|CacheClearResult|SDKPrepareResult|Failure}`です。SDKのreceiptは準備受付の結果であり、jobの最終状態とは別です。unknown/pendingではreceiptを省略します。

### 6.4 要求と成功応答

以下の要求名には全て`Request`を付けます。引数欄の`M`はMutation、`P`はPageの各fieldを展開する略記です。全要求は代わりにFailureを返せます。共通成功型の内容は後述します。

| 要求 | 引数 | 成功型・副作用 |
| --- | --- | --- |
| SessionHello | `protocol:uint32, client_id:Id, expected_project_id:Id?, token:string?` | `HelloResult {protocol, project_id, server_instance_id:Id, limits:Limits, capabilities:[string]}`。tokenはWebSocketで必須。project不一致はpermission_denied |
| ProjectOpen | `view_id:Id?` | `OpenResult {snapshot:ProjectSnapshot, subscription_id:Id, seq:Seq}`。接続に購読を一つ作る |
| ProjectClose | `subscription_id:Id` | Ack。指定購読だけ終了。runや他peerに影響しない。viewは自動保存しない |
| ProjectInspect | なし | ProjectSnapshot。購読・ビルド・Reloadをしない |
| ProjectSave | `M, changes:[DocumentChange]` | SaveResult。transactionを確定 |
| ProjectReload | `M` | SaveResult。許可領域の外部変更を確定文書へ取り込む。差分なしならdeltaなし、revision不変。取り込み失敗時は確定文書を維持し、外部編集そのものを巻き戻さない |
| ProjectValidate | `at_rev:Rev, targets:[CardPath]=[], mode:structure\|executable=executable` | `ValidationResult {rev, valid:bool, diagnostics:[Diagnostic]}`。空targetsは全体。ビルド・実行なし |
| ProjectImport | `M, source_grant:Id, definitions:[string]=[]` | `ImportResult {save:SaveResult, id_map:[{from:string,to:string}]}`。許可した外部定義をコピーし、配置はしない。空配列は全定義 |
| ProjectCacheClear | `op_id:Id` | `CacheClearResult {deleted:[string], retained:[string], freed_bytes:uint64}`。run・SDK生成停止中のみ。自プロジェクトのcacheに限定 |
| OperationGet | `op_id:Id` | `OperationResult {state:unknown\|pending\|completed, receipt:OperationReceipt?}`。completedなら元の成功型かFailureをreceiptに含む |
| ProjectFileList | `path:Path, at_rev:Rev, depth:uint32=0, include_directories:bool=false, P` | `SourcePage {rev, entries:[SourceInfo], next_cursor:string?}`。depthは0〜8、辞書順 |
| ProjectFileStat | `path:Path, at_rev:Rev` | `SourceInfo {path, kind:file\|directory, exists:bool, hash:Hash?, size_bytes:uint64?}` |
| ProjectFileRead | `path:Path, at_rev:Rev, if_hash:Hash?, offset:uint64=0, limit_bytes:uint32=262144` | FileContent。最大1 MiB、raw bytesを取得。編集UI側でUTF-8として復号 |
| ProjectFileFormat | `path:Path, text:string, language:cpp\|python\|bash\|nix\|json\|fbs` | `FormatResult {text:string, diagnostics:[Diagnostic]}`。文書・revisionを変更しない。整形器未提供はunsupported |
| CardSDKPrepare | `card:CardPath, at_rev:Rev, op_id:Id` | `SDKPrepareResult {op_id, rev:Rev, state:pending\|ready, files:[SDKFile]}`。型導出・SDK生成を非同期受付。各SDKFileは読取専用Path・Hash・size_bytes |
| CardSDKStatus | `op_id:Id` | SDKPrepareResultまたはFailure。完了ファイルはFileReadで取得可。通常のInspectで暗黙ビルドしない |
| RunPlan | `at_rev:Rev, card:CardPath, propagate:bool=true` | `RunPlanResult {rev, targets:[CardPath], starts:[CardPath], build_candidates:[CardPath], diagnostics:[Diagnostic]}`。分岐は候補で、実行予測を保証しない |
| Run | `M, card:CardPath, propagate:bool=true, max_concurrency:uint32=4, nix_jobs:uint32=1, nix_cores:uint32=1` | `RunAccepted {op_id, run:RunStatus, replayed:bool}`。同一文書に一run。concurrencyは1〜64、Nix値は1以上かつ環境枠以内 |
| RunCancel | `run_id:Id` | RunStatus。取消中なら同じ状態、終了済みなら最終状態を返す。未知IDはnot_found |
| RunStatus | `run_id:Id?` | RunStatus。省略は実行中、なければ最後のrun。実行歴なしはrun_idなし・state=idle |
| RunHistory | `card:CardPath?, artifacts_only:bool=false, P` | `RunPage {runs:[RunSummary], next_cursor:string?}`。受付順の降順 |
| RunInvocations | `run_id:Id, card:CardPath, P` | `InvocationPage {records:[InvocationSummary], next_cursor:string?}`。呼び出し番号の昇順 |
| RunRecord | `run_id:Id, card:CardPath, invocation:uint64?, follow_latest:bool=false` | RecordResult。invocation指定とfollow_latest=trueは併用不可 |
| RunValueRead | `run_id:Id, card:CardPath, invocation:uint64, side:input\|output, port:string, offset:uint64=0, limit_bytes:uint32=1048576` | FileContent。保存されたポートbufferを分割取得。recordIO無効ならrecording_disabled |
| ArtifactList | `run_id:Id, card:CardPath?, invocation:uint64?, P` | `ArtifactPage {artifacts:[ArtifactInfo], next_cursor:string?}`。invocation指定時はcard必須 |
| ArtifactRead | `artifact_id:Id, member:Path?, offset:uint64=0, limit_bytes:uint32=1048576` | FileContent。Fileはmemberなし、Directoryは配下の通常ファイルを指定 |
| LogRange | `run_id:Id, card:CardPath?, invocation:uint64?, stream:stdout\|stderr\|journal, offset:uint64=0, tail:bool=false, limit_bytes:uint32=65536` | LogSlice。最大256 KiB、invocation指定時はcard必須。tail=trueとoffset≠0は併用不可 |
| ViewSetVisible | `subscription_id:Id, cards:[CardPath], enabled:bool=true` | Ack。購読単位のDisplay配信対象を全置換。Open直後と空cardsは配信なし。enabled=falseはcardsを保持して配信を停止 |
| ViewSave | `view_id:Id, base_view_rev:Rev, view:ViewState` | `ViewSaveResult {view_id, view_rev:Rev}`。viewだけCAS更新、文書rev・execution_revは不変 |

`at_rev`が現在の確定revisionと違えば`revision_conflict`を返します。sourceのdisk hashが確定状態と違えば`external_change`とし、未取り込みの内容を同じrevisionで返しません。必要な長い処理は不変snapshotを保持し、完了結果に対象revを付けます。

Importのsource_grantは起動管理が利用者の選択領域を読取専用で公開して発行するIDです。coreは既知のIDから受付時に固定した元ファイル内容だけを使い、Agentが任意のホストパスを指定して権限を増やせません。ID衝突時は新IDを割り当て、返すid_mapで元との対応を示します。

LogExportはcore RPCに設けません。クライアントがLogRangeで読み出した内容を利用者の選択先へ保存します。成果物の書出しもArtifactReadを使い、coreにmacOSの保存先パスを渡しません。

CacheClear・CardSDKPrepareもop_idで重複実行を防ぎ、OperationGetで受付を照合します。途中停止した処理を成功扱いせず、SDKの完了はCardSDKStatusで確認します。SDKStatusのpendingはfiles空、readyは全filesを持ち、失敗はFailureです。再起動時の未完了jobはinterruptedにし、未存在op_idはnot_foundです。File系で読めるのは確定文書・source manifest・公開SDKだけとし、履歴DB・journal・receipt・token・lockは直接公開しません。SDKの生成パスはcoreが公開した読取専用領域に限定し、Source変更では編集できません。準備対象revを保存し、FileRead時にそのrevが失効していれば再準備させます。起動管理から渡すgrantの登録・失効は認証された起動設定で扱い、通常RPCからは変更できません。

### 6.5 実行・履歴・表示のデータ型

`RunStatus {run_id:Id?, state:RunState, rev:Rev?, execution_rev:Rev?, accepted_at:Time?, started_at:Time?, ended_at:Time?, failure:Failure?}`を定義します。stateは`idle → queued → preparing → running → succeeded|failed|cancelled`、取消途中はcancellingです。途中からfailed/cancellingへ移れます。idle以外はrun_id・rev・execution_rev・accepted_atを必須とし、終了時はended_atを必須にします。ended_atはcoreが最終結果を確定した時刻であり、異常終了したworkerの実停止時刻を推測したものではありません。running相当のboolは保持せず、queued/preparing/running/cancellingから導出します。core再起動で未完了ならfailedと`interrupted`を記録し、自動再実行しません。

RunSummaryはRunStatusに`source_hash:Hash, target:CardPath, propagate:bool`を加えます。Cardの状態はnot_started/waiting/ready/preparing/running/succeeded/failed/blocked/cancelled/skipped、処理段階はevaluate/build/execute/validateとし、runの状態と混同しません。待機・blocked時は`WaitingInput {port:string, upstream:[{card:CardPath,port:string}]}`の配列で不足入力と依存先を返します。

```text
InvocationSummary {
  run_id:Id, card:CardPath, invocation:uint64, state:CardState,
  phase:Phase?, started_at:Time?, ended_at:Time?, duration_ms:Duration?,
  failure:Failure?, waiting_inputs:[WaitingInput]
}
InvocationRecord {
  summary:InvocationSummary, inputs:[NamedValue], outputs:[NamedValue],
  input_sources:[{port:string, card:CardPath, invocation:uint64}],
  display:Display?, record_io:bool
}
RecordResult {
  selected_invocation:uint64?, record:InvocationRecord?,
  run:RunSummary, status:HistoryState,
  aggregate:{calls:uint64, succeeded:uint64, failed:uint64,
             cancelled:uint64, total_duration_ms:Duration},
  exists:bool, recording_disabled:bool, truncated:bool, orphaned:bool
}
NamedValue {
  name:string, kind:File|Directory|FlatBuffer,
  schema_bfbs:bytes, root_type:string, size_bytes:uint64,
  inline_bytes:bytes?, artifact_id:Id?
}
```

invocationはCardごとにrun内で1から増加し、欠番を許可します。RunRecordは番号指定ならその呼び出し、follow_latest=trueなら進行中を含む最新、両方省略なら最新の完了呼び出しを選びます。存在しない番号はexists=false、recordなし。未知run/Cardはnot_foundです。Cardの存在確認は選択runの記録を基準とし、現在文書から削除済みでも履歴を読めます。recording_disabledはI/O非記録、truncatedは取得量の制限、orphanedは現在文書からCardが消えた状態を表し、相互に独立します。

HistoryStateは`{state:CardState, failure:Failure?, time:Time?}`です。指定した過去の呼び出しには別の状態を混ぜません。最新追跡では選択呼び出しの進行statusを使い、それがなければ確定record、未確定でrun終了済みならその失敗・取消を反映します。最新追跡で同Cardの過去の失敗が残る場合はfailedを優先します。選択呼び出しがなければnot_startedです。

I/Oのinline合計は1応答256 KiBまでとし、省略した値はsize_bytesと型情報を残してtruncated=trueにします。保存済みならRunValueReadで取得できます。BFBSなど値以外のmetadataだけでframe上限を超える場合はresource_limitで返し、型情報を欠いた成功応答にしません。recordIO無効時は最後の再実行用入力を全履歴I/Oとして公開しません。File/Directoryのパス文字列は実行環境内の値であり、ホストファイルを開く権限を表しません。

`ArtifactInfo {artifact_id:Id, run_id:Id, card:CardPath, invocation:uint64, output:string, kind:File|Directory, name:string, members:[{path:Path,size_bytes:uint64,hash:Hash}]}`を定義します。Directoryもファイル単位のmanifestを持ち、symlink・範囲外への参照は保存時に拒否します。初版は一成果物1,000ファイルまでとし、超過は保存時にresource_limitで失敗させます。

`FileContent {name:string, hash:Hash, size_bytes:uint64, offset:uint64, next_offset:uint64, bytes:bytes, eof:bool}`は実際の取得範囲を返します。offset=sizeは空bytes/eof=true、offset>sizeはinvalid_range。limit_bytesは1〜1 MiBです。本文の欠落や不正hashはnot_found/integrity_errorで返します。未知ファイルはStatだけexists=false、それ以外の読取はnot_foundです。

`LogSlice {text:string, offset:uint64, next_offset:uint64, size_bytes:uint64, exists:bool, truncated:bool}`を定義します。cursorはrun/card/invocation/streamで選んだ論理ログ内のUTF-8 byte位置です。stdout/stderrはincremental decoderでchunkをまたぐ文字を復元し、不正UTF-8だけを保存時に置換して、変換後bytesを正本にします。通常offsetは文字境界を要求し、末尾は文字を割らない位置まで短くします。tailは末尾からlimit_bytes以内の先頭文字境界へ進めます。次回はnext_offsetから読み、size_bytesはその読取時点の永続化済み末尾です。未作成ログはexists=falseで空、scopeの違うcursorを流用しません。limit_bytesは4〜256 KiBとし、offset=sizeは空、offset>sizeはinvalid_range。truncatedは指定範囲の先頭または末尾を省略した場合にtrueです。journalは1行1JSONで`{time_ms,phase,event,card?,invocation?,message}`を持ち、eventはstarted/output/succeeded/failed/cancelledです。

Pageのcursorは検索条件と最初の読取の上限位置を固定するopaque文字列です。途中で追加されたrun・呼び出し・成果物を同じ走査に混ぜません。既存runの状態更新は各ページ取得時点の値を返します。cursorは最低10分有効とし、条件変更・期限切れはinvalid_cursor。next_cursor省略は末尾です。一覧応答は1 MiBのpayload上限でページを区切り、進むcursorを返し、一項目がこの上限を超える場合はresource_limitにします。

Displayは`{card:CardPath, run_id:Id, invocation:uint64, revision:Seq, value:Text|Scalar|LineChart}`とします。revisionはrun/card単位で単調増加し、UIは古い値を無視します。Textは`{text:string}`、Scalarは`{value:double,label:string}`、LineChartは`{series:[{id:string,label:string,x:[double],y:[double]}]}`です。数値は有限、x/yは同長、合計10,000点・1 MiBまでを初期上限とします。全置換snapshotとして送り、UIが欠落frameを補間する前提にしません。最新のDisplayは可視状態に関係なくcoreが保持し、終了時に履歴へ保存して再接続から取得可能にします。ファイルプレビューは成果物APIを使います。

ViewStateは`{scope:CardPath, cameras:[{scope:CardPath,x:double,y:double,zoom:double}], selected_cards:[CardPath]}`。座標は有限、zoomは正の有限値です。view_idが新規ならbase_view_rev=0、保存ごとにview_revを1増やし、競合はrevision_conflictとcurrent_revに現在のview_revを返します。`.tsugumi/view.json`内でview_id別に保存し、別ウィンドウの表示を上書きしません。

### 6.6 購読・通知・切断復旧

ProjectOpenはsnapshot取得と購読開始を一つの境界として扱います。返信のsnapshotはseq時点の確定文書・run状態を含み、その後の通知は必ずseqより大きくします。Open返信のseqを1とし、最初の通知は2です。Open返信を先に送信queueへ入れ、同時保存の取りこぼしを防いでください。同一接続の再Openは古い購読を置換し、新しいsubscription_idを返します。Inspectは購読を変更しません。

| 通知 | payload・規則 |
| --- | --- |
| DocPatched | DocumentDelta。確定順、永続化後に配信。返信との重複はop_idとrevで判定 |
| RunEvent | `run_id:Id, value:Accepted\|StatusChanged\|CardChanged\|Timeline\|Finished`のunion。Accepted/StatusChangedはRunStatus、CardChangedはInvocationSummary、Timelineは`{card:CardPath,invocation:uint64,phase:Phase,lane:uint32,start:Time,end:Time?}`、FinishedはRunSummary |
| DisplayUpdated | Display。subscriptionの可視対象だけに配信。対象変更時に最新snapshotを送る |
| LogAvailable | `run_id, card:CardPath?, invocation:uint64?, stream, end_offset:uint64`。対象ログの永続化済み末尾を通知 |

カード単位の通知と全体通知で重複処理しないよう、LogAvailableのoffsetは通知に指定したscopeだけで有効とします。異なるscopeのログを表示中なら、その表示自身のnext_offsetから読み直します。

seqは購読ごとに実際の送信時に連番で付けます。DisplayとLogAvailableは採番前に同じ対象の最新へ集約できます。DocPatched・RunEventの確定状態・Finishedは集約せず、送信待ち上限に達したpeerは切断します。Finishedは履歴確定後に送り、同runの最終Displayを先にqueueへ入れます。Run返信とその通知の到着順は仮定しません。

再接続ではHello→Openで新snapshotを取得し、RunStatus・RunInvocations・RunRecord・LogRangeで不足分を読み直します。初版では通知の永続replayは提供しません。Close返信を送信する時点で旧購読の送信待ち通知を除き、その後はその購読から配信しません。未知のsubscription_idへのCloseはAckにします。古いsubscriptionの通知は無視し、seqの欠落またはDeltaのbase_rev不一致なら再Openします。未確定編集は保持し、復旧snapshotへ勝手に上書き保存しません。送信後に返信を失った変更・RunはOperationGetと同一op_idの再送で照合します。

### 6.7 エラーと互換性テスト

FailureKindには少なくとも`invalid_request, protocol_mismatch, unauthenticated, permission_denied, not_found, invalid_path, invalid_range, invalid_cursor, revision_conflict, hash_conflict, external_change, operation_conflict, busy, timeout, resource_limit, invalid_document, validation_failed, input_missing, execution_limit, cancelled, interrupted, nix_failed, worker_failed, output_invalid, persistence_failed, integrity_error, recording_disabled, unsupported, internal_error`を定義します。revision競合にはcurrent_rev、hash競合にはargsのpath・expected_hash・actual_hashを付けます。失敗を人向け文章で判定せず、busy以外も含め自動再試行可能とは推測しません。

Limitsは`{max_frame_bytes:uint32, max_connections:uint32, max_inflight:uint32, mutation_queue_limit:uint32, queue_wait_ms:uint32, outbox_messages:uint32, outbox_bytes:uint64, page_limit:uint32, file_chunk_bytes:uint32, log_chunk_bytes:uint32}`とし、採用した実効値をHelloで返します。

Helloのcapabilitiesは実装済み要求名と表示variantを列挙します。schemaにあるが未実装の要求はunsupported、未提供操作をUIで成功扱いしません。初版で必要なOpen・Inspect・Save・Reload・Validate・OperationGet・ソース読取／一覧／属性・SDK準備／確認・Run関連・履歴／値／成果物／ログ読取・View操作はデモとAgent基盤に合わせて実装します。

FBSの全要求に成功型をコメントし、schema生成・C++/Swift/Python相互のround-trip、必須scalar欠落、明示false/0、64 bit最大値、未知unionを検証します。さらに次を実RPCで試してください。

- 同時OpenとSaveでもsnapshotと通知の間に変更が落ちない。
- 同じbase_revの異なるSaveは片方だけ成功し、同一op_idの同時再送・再起動後再送は一度だけ確定する。
- Run受付直後に切断して再送しても二つ目のrunが生まれず、取消と自然完了の競合でも最終結果が一つになる。
- UTF-8境界・ページ境界・成果物末尾をまたぐ取得で欠落・重複せず、条件違いcursorや外部変更を検出する。
- 低速peer・保存I/O・Nix準備中にも別接続の状態取得と取消を受け付け、UIとAgentが同じ変更契約を使う。

次の往復を契約テストの基本例にします（A・B・R・P・S・KはUUIDの記号表記、要求の他の必須fieldは省略）。

```text
UI    Hello → project=P、protocol=1
UI    Open  → snapshot.rev=10、subscription=S、seq=1
Agent Hello → expected_project_id=P が一致
Agent Save(base_rev=10, op_id=A, changes=[SetProject(...)])
      → SaveResult(delta.base_rev=10, delta.rev=11, replayed=false)
UI    ← DocPatched(subscription=S, seq=2, op_id=A, base_rev=10, rev=11)
Agent 同じSaveを再送 → 元のSaveResult、replayed=true、追加通知なし
UI    Save(base_rev=10, op_id=B, ...) → revision_conflict(current_rev=11)
UI    Run(base_rev=11, op_id=R, card=["begin"], ...) → run_id=K を耐久化
      返信受信前に切断
UI    再接続・Hello・Open → OperationGet(R) → 元のrun_id=K
UI    同じRunを再送 → 同じrun_id=K、replayed=true
UI    RunStatus(K) → 現在状態。二つ目のrunは作らない
```

### 6.8 coreとworker

worker.fbsは外部RPCから独立させ、`WorkerMessage {call_id:uint64, value:WorkerCall|WorkerResult|WorkerFailure|WorkerCancel|WorkerDisplay}`をroot、identifierを`TSW1`とします。各本文は次の通りです。

```text
FlatBufferData { schema_bfbs:bytes, root_type:string, bytes:bytes }
WorkerCall {
  run_id:Id, card:CardPath, invocation:uint64,
  inputs:[{name:string, kind:File|Directory|FlatBuffer, value:FlatBufferData}],
  params:FlatBufferData, outputs:[Port],
  working_directory:string, temporary_directory:string, output_directory:string,
  environment:[{name:string,value:string}], deadline_ms:uint64?
}
WorkerResult { outputs:[{name:string,bytes:bytes}] }
WorkerFailure { failure:Failure }
WorkerCancel {}
WorkerDisplay { display:Display }
```

同じworkerには同時に一呼び出しだけ送り、1以上で再利用しないcall_idで結果を照合します。ResultかFailureの一方だけで終了し、Displayは終了前に複数回送れます。出力型はCallのPortから照合します。期限は残り時間で渡し、coreも期限を監視します。取消は制御socketのWorkerCancelで伝え、1秒の猶予後も終了しなければ子プロセスごと停止します。切断・不正call_id・二重結果はworker_failedです。

初版は4 byte長さ付きのFlatBufferをsocketで渡し、本文上限16 MiBを越える呼び出しはresource_limitにします。大きい値にmmapを導入する場合も独立した完全なbufferと寿命・所有者を明示します。作業先・File/Directory入力はcoreが許可領域に解決したものだけを渡し、workerからのパスやメモリアドレスを無検証で開きません。共通main.so ABIとPython SDKの方式は変更しません。

### 6.9 Agent

AgentはUI側の独立モジュールとし、モデル接続・会話・生成・圧縮・検索・使用量・取消を所有します。これらを`tsugumid`のRPCに追加しません。UIへは要求ID付きの進捗・tool結果・最終回答を非同期で通知し、モデル資格情報はホスト側で保護してcoreやworkerへ渡しません。

core操作用のtool schemaは公開FBS要求と公開属性から生成します。モデルのtool引数・結果とFlatBuffers RPCの相互変換はAgent層が担当し、モデル向けの表現に合わせてcoreの通信形式を変更しません。Agentは通常クライアントとして認証し、許可されたプロジェクトだけを操作します。不完全なtool引数は実行せず、保存はProjectSave、計算はRunなどの同じRPCを使います。任意コード・shellの実行が必要ならコンテナ内の実行経路に渡し、ホストで直接実行しません。

会話・tool要求と返信・op_id・revision・run IDの対応はクライアント側で永続化し、確定した変更と計算結果の正本はcoreに残します。切断後はreceiptや状態を照合し、非冪等な要求を無条件に再送しません。未保存のUI編集をAgentが勝手に確定せず、競合時はその編集を保持します。

Agentの取消はモデル通信と未送信toolを止め、受理済みrunは止めません。計算の取消には明示的なRunCancelを使います。

第一の試作でもAgentの基盤は揃えます。provider設定・認証・モデル能力取得、会話と文脈管理、streaming応答、tool呼出しの組立・検証・実RPC実行・結果返却、取消・エラー表示、会話とtool証跡の保存・再開までを一連の経路として実装してください。文脈の使用量と圧縮、通信再試行と確定済みtoolの重複実行防止も扱います。UIは対話とtoolの進捗を表示し、選択Cardなどの文脈を渡せるようにします。

ローカルprovider fixtureで「プロジェクト読取→設定変更→実行→結果の説明」をUIから通し、途中取消・切断・再開も検証します。fixtureはモデル通信だけを置き換え、core操作は実物のRPCを使います。実provider接続とテスト用fixtureを明確に分け、有料モデルを使うデモは第一試作の条件にしません。


## 7. 第一の試作: UIから触れるデモ

最初の到達点は、UIからデモを開き、設定や配線を編集し、実行・取消・結果確認・保存・再読込まで操作できるアプリです。全RPCの完成を待ってUIを後付けにせず、まず一つのデモを端から端まで通し、次のデモで機能を広げてください。

HomeにDemos一覧を用意し、選択すると同梱原本の編集可能なコピーを開きます。以後の操作はターミナル不要とし、入力欄・実行ボタン・取消・履歴・成果物への導線を揃えます。実行中のCard・選ばれた分岐・ログ・結果が画面で分かり、コピーを再度開いて変更と履歴を復元できることを必須とします。

### 第一試作の必須デモ（3系統）

以下を新規プロジェクトとして同梱してください。必要なコード・定義・Nix・FBS・配置・期待結果を含め、利用者固有の絶対パスや外部サービスに依存させません。

| デモ | 作る処理とUI操作 | 完成の確認 |
| --- | --- | --- |
| データからレポートへ | seed付きCSV生成→Python集計→レポート・Directory成果物→内包グラフで複製。seed・見出しを編集し、入れ子へ移動、内容をプレビュー・書出し | 同じseedで同じ集計。設定変更が結果に反映され、保存・再起動後も成果物を読める |
| コラッツ C++／Python | 同じグラフを両言語で実装。入力→merge→fork→終了・偶奇判定→branch→÷2または3n+1→merge。初期値と反復上限をUIで編集 | 27から111変換で1に到達し、初期値を含む112値を表示。両版の全数列が一致。上限超過と整数overflowは診断 |
| Live Stream | seed付き乱数→折れ線表示とfeedback。既定100ms間隔・200標本・直近60点。間隔・seed・標本数・表示幅を編集 | 実データでグラフが更新され、途中取消できる。再実行で系列を初期化し、最終グラフを保持。並列数1／2でも欠落しない |

各デモに失敗・再実行・取消の検証ケースを組み込みます。レポートでは意図的な失敗のstderrと後段停止、設定修正後の保存入力による単体再実行を確認します。コラッツでは未選択分岐のskippedと上限超過を、Live Streamでは途中取消・再実行を確認します。配置のUndo/Redoで計算が走らないことも確認してください。

標本間隔を作る待機はLive Streamの用途として認めますが、実計算の代わりにsleepで進捗を演出しないでください。期待値は独立した計算でも検証し、デモ用の固定成功応答を作らないでください。

第一の試作ではDisplayを数値・テキスト・折れ線・ファイルプレビューに絞ります。3Dや独自WASM描画、モデル接続を要する文章生成・Draft変換はこの到達点の条件にしません。N-Queens（N=4〜7、N=7で40解、候補単位の約1100呼出しと盤面表示）と200処理Card・10系列×20段の大規模デモは次段階の負荷確認に回し、継続実行・パン・ズーム・選択と並列実行の応答を測ります。後続項目を第一の試作で実装済みとは報告しないでください。

## 8. 検証と納品

第一の試作では、上のデモを実機で通し、操作手順・期待結果・結果画面を残してください。UIの検証に加え、生成RPCクライアントで保存競合・再送・通知、複数ファイル保存の障害復旧、型導出、worker異常、期限・取消、履歴とログを検証します。コンテナ再作成後のデータ保持・未許可ホスト領域へのアクセス拒否・UI再接続も確認してください。同時起動でcoreが一つになること、UIとAgentの競合保存が一方だけ成功すること、長い実行・低速peer・保存I/Oの待機中も状態取得と取消を受け付けること、所有者の異常終了後に安全に再接続・再起動できることも検証します。

Agent基盤のfixtureを通した検証も第一試作に含めます。その後、残るRPCと高度なAgent機能を実装し、過去／最新の状態選択、recordIOと成果物保持、日本語ログのbyte境界・ページング、低速peer・不正frameを検証します。Agentはローカルprovider fixtureから実RPCへ接続し、UIとの同時編集・中断・不完全tool・取消を確認します。coreがDawn・SwiftUI・モデルSDKなしでビルド・動作すること、UIなしのAgentクライアントでも同じRPCを使えることも検証します。

納品物はソース・FBS・Nix/CMake/SwiftPM設定、最小containerイメージと起動設定、起動可能なmacOSアプリ、UIから動くデモ、再現手順を記載したREADMEです。第一の試作と全仕様の完成を区別し、要求ごとの実装済み・未実装・未検証、容量・起動・応答の実測を簡潔に報告してください。

CIの開始・再実行・有効化、自動push／PRトリガー、有料モデル・有料計算、外部公開は明示的な指示なしに行わず、ローカルで確認してください。環境不足は報告し、fixtureだけで実機検証済みと扱わないでください。固定成功・疑似実行・テスト削除で未実装を隠さず、未対応要求には構造化したエラーを返してください。

