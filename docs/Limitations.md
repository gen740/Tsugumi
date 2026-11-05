# まだできないこと

今の Tsugumi にできないこと、保証していないことの一覧です。各文書に書いたものをここに集めています。

## 考え方として保証しないこと

- **科学的な妥当性は保証しません。** Tsugumi が用意するのは、判断の材料に手が届き、別の可能性をすぐに試せる足場までです（[Philosophy](Philosophy.md#きれいな結果だけでは確かめられない)）。
- **同じ環境でも、同じ結果になるとは限りません。** 乱数・外部データ・到着順・保持した状態に結果が依存する処理では、それぞれ条件を決める必要があります（[Q&A の Q3](QA.md#q3-解析の再現性をどのように確保するのですか)）。
- **過去の版に戻っても、当時の状態は戻りません。** 戻るのはコードと定義で、worker の状態や全データは復元しません（[Q&A の Q6](QA.md#q6-git-と実行履歴はそれぞれ何を記録するのですか)）。

## 結果をたどること

- **2 つの実行を並べて比べる画面はありません。** 実行履歴はそれぞれ開いて見ます。
- **版に戻らずに、その版の Canvas と結果を見ることはできません。** 「この版から始める」で戻って見ます（[版の管理](Versions.md#まだできないこと)）。
- **Agent の報告から、根拠の Card や実行へのリンクはありません。** チャットの文章の中で Card を指すリンクは開けません。
- **実行の記録に、ビルド結果の store パスはありません。** 環境は実行の commit（`pkgsPath` と `default.nix` を含む）からたどります。
- **上流を変えたあとの部分実行は、古い入力を使います。** 最後は Workflow 全体を通して確かめます（[Q&A の Q5](QA.md#q5-解析の途中からやり直せることにはどんな意味がありますか)）。
- **`recordIO` を入れていない Card の過去の入出力値は残りません。** 既定は off です（[Q&A の Q4](QA.md#q4-なぜすべての実行データを保存しないのですか)）。

## データと権限

- **Agent に送る内容を自動で伏せる仕組みはありません。** Agent が読んだソース・ログ・入出力値はモデルのプロバイダへ送られます（[モデルに送られるもの](security.md#モデルに送られるもの)、[業務自動化とデータの境界](Vision.md#業務自動化とデータの境界)）。
- **他人のプロジェクトを開くときに、権限の確認は出ません。** `_card.json` に書かれた `hostFolders` は開くときに共有され、`network` と `sandbox: false` は実行時に有効です。確認が出るのは Agent が権限を広げる保存のときだけです。受け取ったプロジェクトは、Card のアイコンと Experimental の項目を確かめてから動かしてください（[既知の制限](security.md#10-既知の制限と注意事項)）。
- **プロジェクト間の隔離は 1 つの Linux カーネルの上です。** カーネルの脆弱性までは防げません。共通の `/nix/store` は全プロジェクトから読めます（[既知の制限](security.md#10-既知の制限と注意事項)）。
- **`network: false` でも、Nix の固定出力 derivation はビルド時に通信できます**（[Nix 評価の制限](security.md#5-nix-評価の制限)）。

## 動く環境

- **アプリは Apple silicon の macOS 26 以降だけです**（[インストール](../INSTALL.md)）。
- **リリース版は ad-hoc 署名で、公証していません。** ブラウザで入れると Gatekeeper に止められます（[インストール](../INSTALL.md#リリース版を入れる)）。
- **v1.0.0 までは版の間の互換性を保ちません。** プロジェクトの文書や設定の移行はしません（[AGENTS.md](../AGENTS.md)）。
- **Web UI には Mac のフォルダの共有がありません。** Seatbelt ではファイルの存在確認（`stat`）を全体で許可し、uid を分けません。`network: true` の Card は同じ LAN や VPN の他の機器に届き、`ssh` は使えません（[Web UI の隔離](WebUI.md#隔離macos-ホスト)）。
- **Claude Code の plugin は Mac アプリに対応していません。** Web UI で開いたプロジェクトだけです（[Claude Code から使う](claude-code.md#対応していないこと)）。Claude Code 自身のシェルやファイル操作は、Tsugumi の権限の確認を通りません。

## 機能

- **Card の言語は C++・Python・bash・R・Julia です。** Go・Rust・Ruby・C# は [#10](https://github.com/gen740/Tsugumi/issues/10)・[#11](https://github.com/gen740/Tsugumi/issues/11)・[#13](https://github.com/gen740/Tsugumi/issues/13)・[#14](https://github.com/gen740/Tsugumi/issues/14) で検討中です。R と Julia の Card は別プロセスで動き、値を JSON でやり取りします（呼び出しごとに起動し、表示は Card の終了時に出ます。常駐 worker の状態保持とライブ表示はなく、R の 2^63 以上の整数は丸められます）。残りは [#12](https://github.com/gen740/Tsugumi/issues/12) にあります。
- **marimo と R Markdown / Quarto の専用 Card はありません**（[#15](https://github.com/gen740/Tsugumi/issues/15)、[#16](https://github.com/gen740/Tsugumi/issues/16)）。
- **図の雛形を選んで作る専用の Visualization Card はありません。** matplotlib・Plotly・Altair はふつうの Python の Card で描けます（[#19](https://github.com/gen740/Tsugumi/issues/19)）。
- **外部 Logger の DB は `sqlite:<name>` だけです**（[実装状況](Development.md#実装状況)）。
- **FBS の自動整形はありません**（同上）。
- **大規模デモ（200 Card）と WASM の表示はありません**（[今後の拡張](Development.md#今後の拡張)）。
- **科学デモの `load` は実行のたびに取得します。** 公開元の内容が変われば結果も変わります。hash で固定しているのは Cas A だけです（[同梱デモのしくみ](Demos.md#科学デモ公開データの取得と解析)）。

## 確認の範囲

- **共通 VM のテストは CI で動きません。** Virtualization.framework を使える Mac で `TSUGUMI_TEST_CONTAINER=1` を付けて行います（[確認範囲](Development.md#確認範囲)、[Evidence](Evidence.md)）。
- **実マウスでの E2E テストはありません。** UI の確認はアプリ内の操作経路で行います（[確認範囲](Development.md#確認範囲)）。
- **Dawn の描画の目視結果は記録していません**（[確認範囲](Development.md#確認範囲)）。
- **実際の Claude API の記録は 1 題材です**（[AgentRun.md](AgentRun.md)）。自動テストはテスト用モデルで行います。

## 配布

- **v0.2.0 までの配布物にはライセンス全文が入っていません。** 次の版で直します（[配布経路で残っている対応](LicenseAudit.md#配布経路で残っている対応)）。
