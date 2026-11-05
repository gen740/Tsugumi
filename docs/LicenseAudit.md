# ライセンス調査

調査日: 2026-11-04。対象: [0ec6460 のソース](https://github.com/gen740/Tsugumi/tree/0ec64607f3a6348386694077e098f85a0f0ca73a)、その lock file に一致する SwiftPM checkout、Web の lock file、Dawn のローカルビルド設定と生成されたリンク規則。

**Tsugumi 自身のコードを MIT で公開する方針は、確認した Tree-sitter の MIT / BSD ライセンスと両立します。ただし、現状の配布物について「ルートの LICENSE 一つで必要な対応が完了している」とは言えません。** 著作権表示とライセンス全文の同梱、フォントの扱い、VM の対応ソース提供に未対応・未確認の部分があります。

この文書はコードとビルド手順から確認できた範囲の記録です。公開済みの全リリースを展開して照合した結果や、VM の全依存 closure の監査完了を示すものではありません。

## Tree-sitter の内訳

| 対象 | 確認した内容 | この変更での対応 |
| --- | --- | --- |
| 実行系 | `tree-sitter` 0.25.10 は MIT、`SwiftTreeSitter` 0.25.0 は BSD-3-Clause | 両者を区別して notices に記載 |
| Nix / Python grammar | `src/` をコピーして使用。各ディレクトリに上流の LICENSE が既にある | 出典と適用範囲を明記 |
| C / C++ / JSON / Bash grammar | SwiftPM で取得してリンク。各上流 LICENSE は MIT | 固定 revision の LICENSE へのリンクと、コピー済み query 用のライセンス全文を追加 |
| highlight query | `Resources/TreeSitter/SOURCES.md` に grammar ごとの出典がある。C++ は C の query も使う | 出典の説明からライセンス全文へ案内を追加 |
| FBS grammar | `grammar.js` はこのプロジェクトのもの。`src/parser.c` と metadata はそこから生成 | 自作 grammar と生成物を区別 |
| `src/tree_sitter/` | generator が配置する外部の補助ヘッダー。自作 FBS grammar のフォルダ内にも存在 | `Grammars/LICENSE.tree-sitter` と適用範囲を追加し、すべて自作と読める説明を訂正 |
| `tree-sitter-swift` | SwiftTreeSitter の `TestTreeSitterSwift` 用。アプリが使う `SwiftTreeSitter` product の依存ではない | アプリに同梱するとした既存の記載を訂正 |

MIT は著作権表示と許諾文の維持を求めます。BSD-3-Clause はバイナリ配布時にも著作権表示・条件・免責文の再掲を求めます。ライセンス名だけの一覧や、ビルドしたマシンにだけ存在する checkout の LICENSE では、配布先への同梱を確認したことにはなりません。原文は [MIT](https://opensource.org/license/mit) と [SwiftTreeSitter の LICENSE](https://github.com/ChimeHQ/SwiftTreeSitter/blob/08ef81eb8620617b55b08868126707ad72bf754f/LICENSE) を参照してください。

## 既存の一覧に不足していたもの

- **Swift Markdown / swift-cmark**: Swift Markdown の `LICENSE.txt` は Swift runtime exception 付き Apache-2.0 です。`NOTICE.txt` もあります。swift-cmark の `COPYING` には BSD-2-Clause に加えて、Houdini・GitHub・utf8proc など由来のコードの MIT 表示があります。パッケージ全体の LICENSE 一つという名前だけで省略せず、全文を保持する必要があります。
- **SwiftMath とフォント**: SwiftMath のコードは MIT ですが、同梱されるフォントまで MIT ではありません。下表を参照してください。
- **hosq**: 事前に開発した自作ライブラリも独立した依存です。`flake.lock` の固定 revision の LICENSE は MIT です。制作範囲の区分にも記載しました。
- **libgit2**: core のリンク対象です。固定した [nixpkgs の定義](https://github.com/NixOS/nixpkgs/blob/1c89083e451e891152365b799595cf7790a43325/pkgs/by-name/li/libgit2/package.nix) は 1.9.7 を指定しています。GPLv2 と特別な linking exception があり、単に「GPL なのでアプリ全体も GPL」と解釈するのは不正確です。[上流の COPYING](https://github.com/libgit2/libgit2/blob/v1.9.7/COPYING) に従って、組み合わせた実行形式とライブラリ自体の配布を区別します。
- **Web の推移的依存**: React などに加えて `loose-envify` / `js-tokens` が lock file にあります。`commander` は KaTeX CLI 用です。lock file に入っているものと、esbuild が実際に出力するものは分けて確認します。
- **Dawn の推移的依存**: Metal 向けのモノリシック静的ライブラリが renderer dylib に入ります。ローカルの `libwebgpu_dawn.a` の Ninja 規則で Abseil の object を確認しました。無効にしている Vulkan / SPIR-V 向け部品を無条件に列挙せず、実際の object とヘッダー由来のコードを追って一覧を完成させる必要があります。

Apache-2.0 の依存については、ライセンスのコピー、適用される NOTICE、変更時の表示なども対象です。[Apache-2.0 §4](https://www.apache.org/licenses/LICENSE-2.0) を参照してください。

## SwiftMath 1.7.3 のフォント

`macOS/build-app.sh` は SwiftPM の resource bundle をコピーします。SwiftMath の `mathFonts.bundle` には 12 個の `.otf` があり、実際に選択しているフォントだけが配布対象になるわけではありません。

以下は固定 revision の [mathFonts.bundle](https://github.com/mgriebling/SwiftMath/tree/fa8244ed032f4a1ade4cb0571bf87d2f1a9fd2d7/Sources/SwiftMath/mathFonts.bundle) に含まれるライセンスファイルと、OTF の `name` テーブル（copyright / license / license URL）を読み取った結果です。

| ファイル | 読み取れたライセンス情報 |
| --- | --- |
| `latinmodern-math.otf` | GUST Font License |
| `texgyretermes-math.otf` | GUST Font License |
| `Asana-Math.otf` | SIL Open Font License 1.1。埋め込み情報に本文と著作権表示あり |
| `Euler-Math.otf` | SIL Open Font License 1.1 |
| `FiraMath-Regular.otf` | SIL Open Font License 1.1 |
| `Garamond-Math.otf` | SIL Open Font License 1.1 |
| `KpMath-Light.otf` | SIL Open Font License 1.1、Reserved Font Name の記載あり |
| `KpMath-Sans.otf` | SIL Open Font License 1.1。copyright フィールドが `bbsans` だけなので上流資料との照合が必要 |
| `LeteSansMath.otf` | SIL Open Font License 1.1。埋め込み情報に本文あり |
| `LibertinusMath-Regular.otf` | SIL Open Font License 1.1 |
| `NotoSansMath-Regular.otf` | SIL Open Font License 1.1 |
| `xits-math.otf` | 埋め込み情報は SIL OFL を基にした条件に言及。bundle の `OFL.txt` は **STIX Font License** という表題で、一般的な OFL 本文と同一とは扱えない |

bundle の `LICENSE` は MathChat の MIT 表示であり、12 フォントの一括ライセンスではありません。`GUST-FONT-LICENSE.txt` は LPPL 1.3c 以降の条件を参照しています。配布する各ファイルに対応する上流の著作権表示、ライセンス、必要な関連文書を揃えることが残っています。特に XITS / KpMath Sans は、ファイル名から許諾を推定せず上流配布物と照合します。

## 配布経路で残っている対応

以下は、この文書追加では解消していない配布上の作業です。

| 優先する対応 | ソース上の根拠 | 完了を確認する方法 |
| --- | --- | --- |
| アプリへライセンス全文・NOTICE・出典をまとめて同梱する | `macOS/build-app.sh` に root `LICENSE` / `THIRD_PARTY_NOTICES.md` をコピーする処理がない。grammar の LICENSE と query の SOURCES は `Package.swift` の exclude に入っている | release ZIP を展開し、実際にリンク・コピーした依存に対応する全文・著作権表示を確認する。Swift の resource bundle に偶然含まれるものだけに頼らない |
| VM に渡す Tsugumi ソースにもライセンスを保持する | `macOS/Container/vm-source.txt` に `LICENSE` / notices がない | allowlist から作った配布用 source ディレクトリに確認用の文書を含める |
| Web の配布物に必要な全文を含める | `web/scripts/build.mjs` は HTML / icon / schema をコピーし、JS を bundle するが、パッケージの LICENSE を集約しない | server / browser それぞれの esbuild metafile と最終 `dist/` を照合する。自動保持されたコメントや `.LEGAL.txt` だけで必要な全文が足りるか確認する |
| Dawn とフォントの一覧・全文を完成させる | Dawn の build graph と SwiftMath resource bundle が、直接依存名だけの一覧より広い | リリースの実際のバイナリ・resource を対象に、版・由来・適用ライセンスを記録して同梱する |
| VM の対応ソースをリリースと結び付けて提供する | `macOS/tools/publish-vm.sh` は kernel / rootfs / version / checksum を公開するが、対応ソースまたは書面による提供申出を作らない | boot image の closure、各ソース・パッチ、生成後の kernel config、ビルド・インストール手順を保存し、採用する GPL/LGPL の提供方法を満たす形で配布する |
| 持ち込んだ kernel config の由来を確定する | `kernel.nix` は Kata ベースの設定と説明するが、元 revision / ファイルへの参照がない | 元の配布物と改変を特定し、必要な出典・表示を保持する |

### VM は「Nix が取得するから対象外」にはならない

アプリが同梱する boot image は Linux、Nix、Bash などとその依存を含みます。一方、`vm-system.nix` の daemon・追加ツールや、各 Card の処理系はインストール後に取得します。この二つを区別する必要があります。

Linux は `kernel.nix` で 6.12.115 を独自設定でビルドしています。`flake.lock` で nixpkgs を固定することや、上流ソースを取得できるコマンドを示すことだけでは、このバイナリに対応するソースを配布したことにはなりません。`kernel.nix` は生成後の config を `$out/config` に保存しますが、現在の VM 配布スクリプトはそれを公開していません。

[GPLv2 §3](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html#section3) は、対応ソースの同梱や条件を満たす書面での提供申出などを定め、ソースにはビルド・インストールを制御するスクリプトも含めています。[LGPLv2.1 §§4–6](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html) も、ライブラリのコピーを配る場合とライブラリを利用するプログラムを配る場合を区別します。動的リンクであることだけをもって、表示・全文同梱・ソース提供やライブラリ差し替えに関する確認が不要になるとは扱いません。

## 文書サイトのフォント

GitHub Pages の図の手書きに使う Yusei Magic（[SIL OFL 1.1](site/assets/YuseiMagic-OFL.txt)）は、図で使う文字だけに絞った WOFF を OFL の全文と一緒に `docs/site/assets/` に置いています。Reserved Font Name の指定がないので、同じ名前のまま改変できます。ページでは `Yusei Magic Subset` として読み込みます。

## 更新時の確認

依存を更新したら、`macOS/Package.resolved`、`web/package-lock.json`、`flake.lock`、Dawn の pin と、ここに記載した版・ライセンス・同梱範囲を照合します。Tree-sitter のコピーしたライセンス全文も上流の同じ版と照合します。公開する ZIP / Web dist / VM image を最後に確認し、ソース上の一覧を更新しただけで配布対応を完了としないようにします。
