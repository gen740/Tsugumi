# まだできないこと

今の Tsugumi にできないこと、保証していないことの一覧です。各文書に書いたものをここに集めています。

ここには 2 種類のものが並んでいます。「考え方として保証しないこと」と「データと権限」の多くは、まだ作っていないものではなく、Tsugumi が何を保証し、何を人間やプロジェクトの作者に委ねるかという設計上の線引きです（[Philosophy](Philosophy.md)、[セキュリティの要点](security.md#どこまでを-tsugumi-が引き受けるか)）。それ以外は、これからの作業です。

## 考え方として保証しないこと

- **科学的な妥当性は保証しません。** Tsugumi が用意するのは、判断の材料に手が届き、別の可能性をすぐに試せる足場までです（[Philosophy](Philosophy.md#きれいな結果だけでは確かめられない)）。
- **環境は固定しますが、計算の中の偶然までは固定しません。** Nix で実行環境を、版でコードと定義を固定するので、プロジェクトさえ共有すれば、一度やった解析は違う Mac でも同じように動きます。ただ、乱数・外部データ・到着順・保持した状態に結果が依存する処理で seed などの条件を決めるのは、計算を書く側です（[Q&A の Q3](QA.md#q3-解析の再現性をどのように確保するのですか)）。
- **過去の版に戻っても、当時の状態は戻りません。** 戻るのはコードと定義で、worker の状態や全データは復元しません（[Q&A の Q6](QA.md#q6-git-と実行履歴はそれぞれ何を記録するのですか)）。

## 結果をたどること

- **2 つの実行を並べて比べる画面はありません。** 実行履歴はそれぞれ開いて見ます。
- **版に戻らずに、その版の Canvas と結果を見ることはできません。** 「この版から始める」で戻って見ます（[版の管理](Versions.md#まだできないこと)）。
- **Agent の報告から、根拠の Card や実行へのリンクはありません。** チャットの文章の中で Card を指すリンクは開けません。
- **実行の記録に、ビルド結果の store パスはありません。** 環境は実行の commit（`pkgsPath` と `default.nix` を含む）からたどります。
- **上流を変えたあとの部分実行は、古い入力を使います。** 最後は Workflow 全体を通して確かめます（[Q&A の Q5](QA.md#q5-解析の途中からやり直せることにはどんな意味がありますか)）。
- **`recordIO` を入れていない Card の過去の入出力値は残りません。** 既定は off です（[Q&A の Q4](QA.md#q4-なぜすべての実行データを保存しないのですか)）。

## データと権限

- **Agent に送る内容を自動で伏せる仕組みはありません。** Agent が読んだソース・ログ・入出力値はモデルのプロバイダへ送られます。送るのは Agent が読んだものだけで、Agent を使わなくてもすべての操作は UI からできます（[モデルに送られるもの](security.md#モデルに送られるもの)、[業務自動化とデータの境界](Vision.md#業務自動化とデータの境界)）。Web 検索をオンにすると検索語も検索サービスへ出て、そこに未公開のデータが載らないことは保証できません（[Web 検索](security.md#web-検索)、既定はオフ）。
- **プロジェクトを開くことは、その中身を信頼するという判断です。** 他人のプロジェクトの `_card.json` に書かれた `hostFolders` は開くときに共有され、`network` と `sandbox: false` は実行時に有効になり、確認は出ません。`make` や notebook を動かすのと同じく、作者が書いたとおりに動かすのが Tsugumi の役目で、作者を信頼するかどうかは利用者が決めます。Tsugumi は、書かれた権限を Card のアイコンと Experimental の項目に出し、`.tsugumi/` を使う前に検査し、Agent が権限を広げる保存には承認を求めます。受け取ったプロジェクトは、それらを確かめてから動かしてください（[どこまでを Tsugumi が引き受けるか](security.md#どこまでを-tsugumi-が引き受けるか)、[既知の制限](security.md#10-既知の制限と注意事項)）。
- **プロジェクト間の隔離は 1 つの Linux カーネルの上です。** コンテナと同じ隔離のしかたで、カーネルの脆弱性までは防げません（Mac との間は VM で分けています）。共通の `/nix/store` は全プロジェクトから読めます（[既知の制限](security.md#10-既知の制限と注意事項)）。
- **`network: false` でも、Nix の固定出力 derivation はビルド時に通信できます。** Nix の仕様です。取得した中身は hash で照合されるので変わりません（[Nix 評価の制限](security.md#5-nix-評価の制限)）。

## 動く環境

- **Windows ではまだ一度も動かしていません。** `cli/install.ps1` と WSL 向けの処理は書いてありますが、確かめたのは PowerShell の構文と代役を使ったテストだけです（[インストール](../INSTALL.md#ブラウザで使うtsugumi-コマンド)）。
- **Linux で確かめたのは OrbStack の VM（Ubuntu 26.04、aarch64）だけです。** x86_64 は CI のビルドだけで、実際には動かしていません。Linux と Windows には Nix が必要です。
- **デスクトップアプリは Apple silicon の macOS 26 以降だけです**（[インストール](../INSTALL.md)）。Linux と Windows では Web UI を使います。
- **リリース版は ad-hoc 署名で、公証していません。** ブラウザで入れると Gatekeeper に止められます（[インストール](../INSTALL.md#デスクトップアプリmacを入れる)）。
- **v1.0.0 までは版の間の互換性を保ちません。** プロジェクトの文書や設定の移行はしません（[AGENTS.md](../AGENTS.md)）。
- **Mac の共通 VM を使えるのは 1 つのアプリだけです。** `tsugumi` コマンドとデスクトップ版を同時には使えず、後から始めた方がその旨を表示します（[tsugumi コマンド](WebUI.md#tsugumi-コマンド)）。会話の履歴・Skill のフォルダ・ChatGPT のサインインは両者で別です。
- **`tsugumi` コマンドは自動では更新しません。** `tsugumi update` で最新版に入れ替えます。デスクトップ版と版が違うと、VM は切り替えのたびにその版の Tsugumi を組み立て直します（Nix のキャッシュが効くので 2 回目からは短くなります）。
- **Mac のホストで直接動かす Web UI には Mac のフォルダの共有がありません。** Seatbelt ではファイルの存在確認（`stat`）を全体で許可し、uid を分けません。`network: true` の Card は同じ LAN や VPN の他の機器に届き、`ssh` は使えません（[Web UI の隔離](WebUI.md#隔離macos-ホスト)）。
- **Claude Code の plugin は Mac アプリに対応していません。** Web UI で開いたプロジェクトだけです（[Claude Code から使う](claude-code.md#対応していないこと)）。Claude Code 自身のシェルやファイル操作は、Tsugumi の権限の確認を通りません。

## 機能

- **Card の言語は C++・Python・bash・R・Julia です。** Go・Rust・Ruby・C# は [#10](https://github.com/gen740/Tsugumi/issues/10)・[#11](https://github.com/gen740/Tsugumi/issues/11)・[#13](https://github.com/gen740/Tsugumi/issues/13)・[#14](https://github.com/gen740/Tsugumi/issues/14) で検討中です。R と Julia の Card は別プロセスで動き、値を JSON でやり取りします（呼び出しごとに起動し、表示は Card の終了時に出ます。常駐 worker の状態保持とライブ表示はなく、R の 2^63 以上の整数は丸められます）。残りは [#12](https://github.com/gen740/Tsugumi/issues/12) にあります。
- **marimo と R Markdown / Quarto の専用 Card はありません**（[#15](https://github.com/gen740/Tsugumi/issues/15)、[#16](https://github.com/gen740/Tsugumi/issues/16)）。
- **図の雛形を選んで作る専用の Visualization Card はありません。** matplotlib・Plotly・Altair はふつうの Python の Card で描けます（[#19](https://github.com/gen740/Tsugumi/issues/19)）。
- **外部 Logger の DB は `sqlite:<name>` だけです**（[実装状況](Development.md#実装状況)）。
- **アプリで同時に開けるプロジェクトは 16 個までです。** 共通 VM の共有デバイスの数で決まります（`SharedVM.slotCount`）。
- **開いていたプロジェクトは、次の起動で開き直しません。** 起動すると Home のウインドウが 1 つ開きます。
- **Web UI では、同じプロジェクトを開いた別のタブを前面に出せません。** ブラウザにその手段がないので、もう 1 つのタブで開きます。
- **FBS の自動整形はありません**（同上）。
- **大規模デモ（200 Card）と WASM の表示はありません**（[今後の拡張](Development.md#今後の拡張)）。
- **科学デモの `load` は実行のたびに取得します。** 公開元の内容が変われば結果も変わります。hash で固定しているのは Cas A だけです（[同梱デモのしくみ](Demos.md#科学デモ公開データの取得と解析)）。

## 確認の範囲

- **共通 VM のテストは CI で動きません。** Virtualization.framework を使える Mac で `TSUGUMI_TEST_CONTAINER=1` を付けて行います（[確認範囲](Development.md#確認範囲)、[Evidence](Evidence.md)）。
- **実マウスでの E2E テストはありません。** UI の確認はアプリ内の操作経路で行います（[確認範囲](Development.md#確認範囲)）。
- **Dawn の描画の目視結果は記録していません**（[確認範囲](Development.md#確認範囲)）。
- **実際の Claude API の記録は 1 題材です**（[AgentRun.md](AgentRun.md)）。自動テストはテスト用モデルで行います。

## 配布

- **v0.2.0 までの配布物にはライセンス全文が入っていません。** v0.3.0 からは同梱しています（[配布経路で残っている対応](LicenseAudit.md#配布経路で残っている対応)）。
