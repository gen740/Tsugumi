# 今後の予定と、保証の範囲

今の Tsugumi でできることの範囲と、これから加わる予定のものをまとめています。各文書に書いたものをここに集めています。

「Tsugumi が原理的に保証できないこと」は、どんな道具でも根本的には解決できないことです（[Philosophy](Philosophy.md)）。「データと権限」は、Tsugumi が何を引き受け、何をプロジェクトの作者と利用者に委ねるかという線引きです（[セキュリティの要点](security.md#どこまでを-tsugumi-が引き受けるか)）。それ以外は、今の範囲と今後の開発予定です。

## Tsugumi が原理的に保証できないこと

- **科学的に正しいかどうかは、道具には保証できません。** それを判断するのは人間です。Tsugumi が用意するのは、判断の材料に手が届き、別の可能性をすぐに試せる足場です（[Philosophy](Philosophy.md#きれいな結果だけでは確かめられない)）。
- **計算そのものに含まれる偶然は、環境を固定しても固定できません。** Nix で実行環境を、版でコードと定義を固定するので、プロジェクトさえ共有すれば、一度やった解析は違う Mac でも同じように動きます。乱数・外部データ・到着順・保持した状態に結果が依存する処理で seed などの条件を決めるのは、計算を書く側です（[Q&A の Q3](QA.md#q3-解析の再現性をどのように確保するのですか)）。
- **過去の版に戻せるのは、コードと定義です。** 当時の worker のメモリの中身や、外から取ってきたデータまでは取り戻せません（[Q&A の Q6](QA.md#q6-git-と実行履歴はそれぞれ何を記録するのですか)）。

## 結果をたどること

- **2 つの実行を並べて比べる画面は、[#156](https://github.com/gen740/Tsugumi/issues/156) で開発予定です。** 今は実行履歴をそれぞれ開いて比べます。
- **Agent の報告から根拠の Card や実行へ飛べるリンクは、[#157](https://github.com/gen740/Tsugumi/issues/157) で開発予定です。**
- **実行の記録に各 Card のビルド結果（store パス）を残すことは、[#158](https://github.com/gen740/Tsugumi/issues/158) で開発予定です。** 今は実行の commit（`pkgsPath` と `default.nix` を含む）から環境をたどれます。
- **部分実行は、保存済みの入力で動きます。** 上流を変えたあとは、最後に Workflow 全体を通して確かめます（[Q&A の Q5](QA.md#q5-解析の途中からやり直せることにはどんな意味がありますか)）。
- **過去の入出力値を残すかどうかは、Card ごとに選べます。** `recordIO` を入れた Card の値が残ります（既定は off。[Q&A の Q4](QA.md#q4-なぜすべての実行データを保存しないのですか)）。

## データと権限

- **Agent に送られるのは、Agent が読んだものだけです。** ソース・ログ・入出力値のうち Agent が読んだものはモデルのプロバイダへ送られ、自動で伏せる仕組みはありません。Agent を使わなくても、すべての操作は UI からできます（[モデルに送られるもの](security.md#モデルに送られるもの)、[業務自動化とデータの境界](Vision.md#業務自動化とデータの境界)）。Web 検索は既定でオフで、オンにすると検索語も検索サービスへ出ます（[Web 検索](security.md#web-検索)）。
- **プロジェクトを開くことは、その中身を信頼するという判断です。** 他人のプロジェクトの `_card.json` に書かれた `hostFolders` は開くときに共有され、`network` と `sandbox: false`、`tsugumi.json` の `nixSources` は実行時に有効になり、確認は出ません。`make` や notebook を動かすのと同じく、作者が書いたとおりに動かすのが Tsugumi の役目で、作者を信頼するかどうかは利用者が決めます。Tsugumi は、書かれた権限を Card のアイコンと Experimental の項目に出し、`.tsugumi/` を使う前に検査し、Agent が権限を広げる保存には承認を求めます（[どこまでを Tsugumi が引き受けるか](security.md#どこまでを-tsugumi-が引き受けるか)、[既知の制限](security.md#10-既知の制限と注意事項)）。
- **プロジェクトどうしは、コンテナと同じ方法で分けています。** 1 つの Linux カーネルの上なので、カーネルの脆弱性までは防げません（Mac との間は VM で分けています）。共通の `/nix/store` は全プロジェクトから読めます（[既知の制限](security.md#10-既知の制限と注意事項)）。
- **Nix の固定出力 derivation は、`network: false` でもビルド時に取得できます。** Nix の仕様で、取得した中身は hash で照合されるので変わりません（[Nix 評価の制限](security.md#5-nix-評価の制限)）。

## 動く環境

- **デスクトップアプリは、Apple silicon の macOS 26 以降で動きます。** Linux と Windows ではブラウザの Web UI（`tsugumi` コマンド）を使います（[インストール](../INSTALL.md)）。
- **Mac 版は、README のコマンド 1 行で入れます。** リリース版は Apple の公証をしていない ad-hoc 署名なので、ブラウザでダウンロードしたものは Gatekeeper に止められます。`curl` で入れれば止められず、以後の自動更新はチェックサムとコード署名を確かめてから入れます（[インストール](../INSTALL.md#デスクトップアプリmacを入れる)）。
- **Linux は Ubuntu と NixOS で確かめています。** aarch64 は OrbStack の VM（Ubuntu 26.04）で、x86_64 は友人の Ubuntu と、SSH 越しに Web UI を使う NixOS のマシンで動かしました（[SSH 越しに使う](WebUI.md#別のマシンで動かしてssh-越しに使う)）。x86_64 では CI（GitHub Actions の Ubuntu）でもビルドとテストを流しています。Linux と Windows には Nix が必要です。
- **Windows は WSL2 で動きます。** 友人の Windows で、WSL2 の中に Nix を daemon なしで入れ、`tsugumi` を入れてデモを開き、実行するところまで確かめました（[#159](https://github.com/gen740/Tsugumi/issues/159)）。`install.ps1` のほか、WSL の Linux のシェルで Linux と同じ手順でも入れられます（[インストール](../INSTALL.md#ブラウザで使うtsugumi-コマンド)）。
- **安定版は v1.0.0 からです。** いまの 0.x の版は開発を速く進めるための版で、版の間でプロジェクトの文書や設定の互換性を保たず、移行もしません（[AGENTS.md](../AGENTS.md)）。
- **Claude Code の plugin は Web UI で使います。** `tsugumi` コマンドなどで開いた Web UI のプロジェクトを、Claude Code に任せられます。Mac と Windows（WSL2）で、plugin からプロジェクトを開き、Card を作って実行するところまで確かめました（[Claude Code から使う](claude-code.md#対応していないこと)）。Claude Code 自身のシェルやファイル操作は、Tsugumi の権限の確認を通りません。

## 機能

- **Card は C++・Python・bash・R・Julia で書けます。** Go・Rust・Ruby・C# は [#10](https://github.com/gen740/Tsugumi/issues/10)・[#11](https://github.com/gen740/Tsugumi/issues/11)・[#13](https://github.com/gen740/Tsugumi/issues/13)・[#14](https://github.com/gen740/Tsugumi/issues/14) で開発予定です。R と Julia の Card は別プロセスで動き、値を JSON でやり取りします（呼び出しごとに起動し、表示は Card の終了時に出ます）。常駐 worker での状態保持とライブ表示は [#161](https://github.com/gen740/Tsugumi/issues/161) で開発予定です。
- **図は、ふつうの Python の Card で描けます。** matplotlib・Plotly・Altair のコードをそのまま書けば、Card の表示になります。雛形を選んで図を作る専用の Visualization Card は [#19](https://github.com/gen740/Tsugumi/issues/19) で登場予定です。
- **marimo と R Markdown / Quarto の専用 Card は、[#15](https://github.com/gen740/Tsugumi/issues/15)・[#16](https://github.com/gen740/Tsugumi/issues/16) で登場予定です。**
- **logger Card は、受け取った値を Card のログと表示に出します。** 外部 DB（SQLite・PostgreSQL など）への記録は [#160](https://github.com/gen740/Tsugumi/issues/160) で開発予定です。
- **アプリでは、16 個のプロジェクトを同時に開けます。** 共通 VM の共有デバイスの数で決まります（`SharedVM.slotCount`）。
- **大規模デモ（200 Card）と WASM の表示は、今後の拡張です**（[今後の拡張](Development.md#今後の拡張)）。
- **科学デモの `load` は、実行のたびに公開元から取得します。** 公開元の内容が変われば結果も変わります。hash で固定しているのは Cas A です（[同梱デモのしくみ](Demos.md#科学デモ公開データの取得と解析)）。

## 確認の範囲

- **共通 VM のテストは、Virtualization.framework を使える Mac で行います**（`TSUGUMI_TEST_CONTAINER=1`）。GitHub Actions の runner では動かせないためです（[確認範囲](Development.md#確認範囲)、[Evidence](Evidence.md)）。
- **実際の Claude API や ChatGPT のプランでも、多くの場面で試しています。** それでも、試していない組み合わせが残っているかもしれません。自動テストはテスト用モデルで行い、実際の API で通した記録の一例は [AgentRun.md](AgentRun.md) にあります。
- **Web 検索は、テスト用モデルで自動テストしています。** 実際の API と ChatGPT プランでは、これから確かめます（[実装状況](Development.md#実装状況)）。
