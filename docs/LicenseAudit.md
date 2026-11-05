# ライセンス調査

調査日: 2026-11-04（[0ec6460 のソース](https://github.com/gen740/Tsugumi/tree/0ec64607f3a6348386694077e098f85a0f0ca73a)、その lock file に一致する SwiftPM checkout、Web の lock file、Dawn のローカルビルド設定と生成されたリンク規則）。2026-11-05 に、公開中の [v0.2.0](https://github.com/gen740/Tsugumi/releases/tag/v0.2.0) の ZIP と VM イメージを展開して照合し、残っていた対応を進めました。

**Tsugumi 自身のコードを MIT で公開する方針は、確認した Tree-sitter の MIT / BSD ライセンスと両立します。** アプリの配布物に入る物のライセンス全文は、次のリリースから同梱されます（[配布経路で残っている対応](#配布経路で残っている対応)）。**VM の GPL / LGPL ソフトウェアに対応するソースは、起動ディスクのリリースに置きます**（[VM の対応ソースの提供方法](#vm-の対応ソースの提供方法)）。

この文書はコード・ビルド手順・成果物から確認できた範囲の記録で、法的な結論ではありません。

## どこまで厳密にするか

個人が MIT で公開し、ad-hoc 署名の ZIP を GitHub で配っているプロジェクトとして、次の線で分けています。

| 扱い | 対象 | 理由 |
| --- | --- | --- |
| 実物で確かめ、機械的に検査する | アプリと VM に入る物の全文・表示。アプリが実際にリンク・コピーする依存の一覧 | ライセンスが配布の条件として明示しており、費用も小さい。人手の確認は依存の更新で必ず古くなる |
| 宣言と上流の文書に任せる | VM の各パッケージのライセンス名（nixpkgs の `meta.license`）と、そのソースのルートにあるライセンス文書 | 93 パッケージのソースを 1 ファイルずつ監査する費用に見合わない。宣言であって監査ではないと明記する |
| 減らして解消する | 使っていない数式フォント 11 個 | 上流との照合より、配らないほうが確実で安い |
| 追わない（理由を残す） | plotly.js の minified build に入っている依存の全文 | 下の「Plotly の依存」を参照 |
| 作者が決める | VM の対応ソースの提供方法、公開済みリリースの扱い（どちらも決定済み、下記） | 費用と責任を負うのが作者で、法的判断を含む |

## 配布物の照合結果（v0.2.0）

`Tsugumi.zip` を展開して確かめた、対応前の状態です。ライセンス文書は SwiftMath の resource bundle に入っていた 3 つ（`LICENSE`、`OFL.txt`、`GUST-FONT-LICENSE.txt`）だけで、root の `LICENSE` も `THIRD_PARTY_NOTICES.md` もありませんでした。`tsugumi-source/` にも `LICENSE` はありません。v0.1.0・v0.1.2 は展開していませんが、そのタグの `build-app.sh` にも root の `LICENSE` をコピーする処理はなく、`vm-source.txt` にも載っていません。

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
- **Dawn の推移的依存**: Metal 向けのモノリシック静的ライブラリが renderer dylib に入ります。下の「Dawn が実際に取り込むもの」で追いました。

Apache-2.0 の依存については、ライセンスのコピー、適用される NOTICE、変更時の表示なども対象です。[Apache-2.0 §4](https://www.apache.org/licenses/LICENSE-2.0) を参照してください。

## Dawn が実際に取り込むもの

Dawn `1716e88` の `libwebgpu_dawn.a`（652 object）について、`ninja -t deps` が記録したコンパイル入力のうち SDK 以外の 1547 ファイルを調べました。`third_party/` から入るのは Abseil だけで、SPIRV-Tools などは入りません。

| 入るもの | ファイル数 | ライセンス | 同梱する全文 |
| --- | --- | --- | --- |
| Dawn / Tint 本体と生成コード | 1244 | BSD-3-Clause | `dawn.LICENSE` |
| `gen/include/dawn/webgpu.h`（生成ファイル。webgpu-headers と同じ表示を持つ） | 1 | BSD-3-Clause（"WebGPU native" developers） | `webgpu-headers.LICENSE` |
| `src/dawn/common/LinkedList.h`（Chromium からの改変コピー） | 1 | BSD-style。参照先の Chromium の LICENSE は Dawn のソースにない | `chromium.LICENSE`（[third-party/chromium/](../third-party/chromium/README.md)） |
| Abseil（`af5aede`、Chromium の取り込み） | 301 | Apache-2.0。NOTICE はない | `abseil-cpp.LICENSE` |

各ファイル冒頭の著作権表示の種類で分類した結果です。Dawn の `LICENSE` にある Apache-2.0 の節は `generator/templates/art/*` と `tools/android/webgpu/*` 向けで、ここには入りません。Dawn の pin を変えたら、renderer をビルドしたあと `ninja -C build-renderer -t deps` で `third_party/` の内訳を見直します。

## SwiftMath 1.7.3 のフォント

アプリが数式の描画に使うのは、`MTMathUILabel` の既定である Latin Modern Math だけです（`macOS/Sources` はフォントを選んでいません。[test_licenses.py](../tests/python/test_licenses.py) で検査）。フォントは必要になったときに読み込まれ、[SwiftMath の `MathFont.swift`](https://github.com/mgriebling/SwiftMath/blob/fa8244ed032f4a1ade4cb0571bf87d2f1a9fd2d7/Sources/SwiftMath/MathBundle/MathFont.swift) は他のファイルに触れません。そこで `macOS/build-app.sh` は、これ以外の 11 個を bundle から取り除きます。v0.2.0 までの配布物には 12 個すべてが入っています。Latin Modern だけを残した bundle で `MTMathUILabel` が数式を組めることは、同じ呼び方の小さな実行ファイルで確かめました。

Latin Modern Math は上流パッケージの説明によると GUST Font License で、これは LPPL の一形態です。[LPPL 1.3c](https://www.latex-project.org/lppl/lppl-1-3c.txt) の条項 2 は Work の一部だけの配布を改変として扱うため、SwiftMath の bundle にある `.otf` と license だけでは足りない読み方ができます。bundle の `latinmodern-math.otf` は上流 [latinmodern-math-1959.zip](https://www.gust.org.pl/projects/e-foundry/lm-math/download/latinmodern-math-1959.zip) の中のものと SHA-256 が一致するので、1 MB に満たない上流パッケージ全体を nixpkgs の固定した `lmmath.src` からそのまま `licenses/fonts/latin-modern-math/` に同梱します。

以下は記録として残す、固定 revision の [mathFonts.bundle](https://github.com/mgriebling/SwiftMath/tree/fa8244ed032f4a1ade4cb0571bf87d2f1a9fd2d7/Sources/SwiftMath/mathFonts.bundle) に含まれるライセンスファイルと、OTF の `name` テーブル（copyright / license / license URL）を読み取った結果です。

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

bundle の `LICENSE` は MathChat の MIT 表示であり、12 フォントの一括ライセンスではありません。`GUST-FONT-LICENSE.txt` は LPPL 1.3c 以降の条件を参照しています。XITS と KpMath Sans の条件は上流と照合していません。配らなくなるので、照合はしないことにしました。別のフォントを使うようにするときは、その時点で照合して同梱します。

## ヘルプ > Tsugumi を知る のフォント

図の手書きに使う Yusei Magic（[SIL OFL 1.1](../macOS/Resources/explainer/YuseiMagic-OFL.txt)）は、`macOS/build-app.sh` が OFL の全文と一緒に `Contents/Resources/explainer/` へコピーします。改変はせず、アプリのプロセスにだけ登録します。単体での販売や、改変版での予約フォント名の使用はしません。由来と hash は [SOURCES.md](../macOS/Resources/explainer/SOURCES.md) にあります。

## 配布経路で残っている対応

| 事項 | 状態 | 対応と確かめる場所 |
| --- | --- | --- |
| アプリへライセンス全文・NOTICE・出典をまとめて同梱する | **解消**（次のリリースから） | [bundle-licenses.sh](../macOS/tools/bundle-licenses.sh) が、ビルドに使った checkout の文書を `Contents/Resources/licenses/` にコピーする。足りなければビルドが止まる。公開の前に [check-app-licenses.sh](../macOS/tools/check-app-licenses.sh) が完成したアプリを検査する（`release.yml`）。CI でも同じコピーを実行する |
| VM に渡す Tsugumi ソースにもライセンスを保持する | **解消** | `macOS/Container/vm-source.txt` に追加。上の検査の対象 |
| Web の配布物に必要な全文を含める | **解消** | `web/scripts/build.mjs` が esbuild の metafile から実際に bundle したパッケージを取り、viewer のライブラリと一緒に全文を `dist/licenses/` へコピーする。[test_licenses.py](../tests/python/test_licenses.py) で検査 |
| Dawn の一覧・全文を完成させる | **解消** | 上の「Dawn が実際に取り込むもの」 |
| フォントの一覧・全文を完成させる | **解消** | 上の「SwiftMath 1.7.3 のフォント」。配るのは Latin Modern Math と Yusei Magic だけになる |
| 持ち込んだ kernel config の由来を確定する | **解消** | Kata Containers 3.17.0 の kernel に埋め込まれた config と 1 バイトも違わないことを確認した（[SOURCES.md](../macOS/Container/kernel/SOURCES.md)） |
| VM の中身の一覧とライセンス全文 | **解消** | [third-party/vm-boot/](../third-party/vm-boot/README.md)。イメージの `/store-paths` と derivation graph から生成し、アプリと VM のリリースに同梱する。別のイメージを配ろうとすると検査が止める |
| VM の対応ソースをリリースと結び付けて提供する | **解消** | 下の「VM の対応ソースの提供方法」 |
| 公開済みのリリースの扱い | **作者が決定（次の版で直す）** | 下の「公開済みのリリース」 |
| plotly.js の bundle に入っている依存の全文 | **追わない（作者が変えられる）** | 下の「Plotly の依存」 |

### VM の対応ソースの提供方法

作者の決定: **起動ディスクのリリース（[gen740/tsugumi-vm](https://github.com/gen740/tsugumi-vm)）に一式を置きます。** `macOS/tools/publish-vm.sh` が、ディスクごとに `sources.tar.gz`（現行のディスクで 754 MiB）と `licenses.tar.gz` を、バイナリと同じリリースへ置きます。アプリの `licenses/vm-boot/README.md` にその URL があり、ソースが公開されていないディスクを載せたアプリは、リリースの検査（`check-app-licenses.sh`）で止まります。

一式の中身は [vm-source-bundle.py](../macOS/tools/vm-source-bundle.py) が `inventory.json` から作ります。

- 各パッケージのソースと、取得するパッチ（126 件）
- 固定した revision の nixpkgs。各パッケージの設定・ビルド・インストールの手順と、nixpkgs の中にあるパッチを含みます
- ディスクを組み立てる Tsugumi の部分（`flake.nix`、`macOS/Container/` など）と、ビルドの手順

展開した一式を使って `vm-boot` を評価すると、公開中のディスクと同じストアパスになることを確かめました。アーカイブの一部（38 件）は `inventory.json` のハッシュとも照合しました。実際に一からビルドし直して、同じバイナリになるかは確かめていません。

| 確認した事実 | |
| --- | --- |
| 配っている物 | `Tsugumi.zip` の中と gen740/tsugumi-vm のリリースの `vmlinux` と `rootfs.erofs` |
| 中身 | 103 のストアパスと kernel で、93 パッケージ。nixpkgs の宣言で GPL か LGPL を含むものは 37 パッケージ（Linux・BusyBox・Bash・coreutils・glibc・Nix など）。GPL-3.0 のものもあるので [GPLv3 §6](https://www.gnu.org/licenses/gpl-3.0.html#section6) も関係する |
| kernel の生成後の config | バイナリの中（`CONFIG_IKCONFIG`）と `third-party/vm-boot/kernel-config` |

残る論点は 2 つです。法的な結論ではありません。

- **置き場所:** [GPLv2 §3](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html#section3) は、ダウンロードで配るなら「同じ場所」からソースを取れれば、ソースの配布として扱うと定めています。`vmlinux` の単体は同じリリースにありますが、`Tsugumi.zip` は gen740/Tsugumi のリリースにあり、別のリポジトリです。GPLv3 §6(d) は案内があれば別のサーバーでよいと明記していますが、GPLv2 にはその明記がありません。同じ持ち主の同じ GitHub で、アプリの中に URL があるので、作者はこの形を選びました。
- **保管:** バイナリを配っている間は、そのソースを消しません。ディスクのリリースを整理するときは、そのディスクを載せたアプリのリリースも確かめます。

### 公開済みのリリース

作者の決定: **次の版で直します。** v0.1.0・v0.1.2・v0.2.0 は取り下げません。

版番号はライセンスの条件に関係しないので、v1.0.0 より前であることは対応しない理由になりません。これらの ZIP にはライセンス全文が入っていないままです。3 つのタグの nixpkgs の revision と `vm-boot.nix` は現在と同じで、v0.2.0 の VM は同じディスクだと展開して確かめました（v0.1.x は展開していません）。したがって、VM のソースは今回の公開でこれらの版にも対応します。

### Plotly の依存

`plotly.min.js` は上流が公開した minified build で、そのまま配っています。全体の `LICENSE` と一部の依存のコメントは残っていますが、中の依存の全文はありません。参考に上流の `plotly.js@3.6.0` の依存を解決すると 258 パッケージ、うち 31 はライセンスファイルがありません。実際に bundle に入るのはこの一部で、上流の metafile がないのでどれかは分かりません。

無関係なものを含む一覧を手で抱えるより、上流の配布形態のまま配ることにしました。厳密にするなら、依存の解決結果から全文を生成して同梱する（約 270 KB と、31 件の手作業）か、plotly.js を自分で bundle して metafile を取る方法があります。Vega は lock file に依存があるので、全文を同梱しています。

### VM は「Nix が取得するから対象外」にはならない

アプリが同梱する boot image は Linux、Nix、Bash などとその依存を含みます。一方、`vm-system.nix` の daemon・追加ツールや、各 Card の処理系はインストール後に取得します。この二つを区別する必要があります。

Linux は `kernel.nix` で 6.12.115 を独自設定でビルドしています。`flake.lock` で nixpkgs を固定することや、上流ソースを取得できるコマンドを示すことだけでは、このバイナリに対応するソースを配布したことにはなりません。

[GPLv2 §3](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html#section3) は、対応ソースの同梱や条件を満たす書面での提供申出などを定め、ソースにはビルド・インストールを制御するスクリプトも含めています。[LGPLv2.1 §§4–6](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html) も、ライブラリのコピーを配る場合とライブラリを利用するプログラムを配る場合を区別します。動的リンクであることだけをもって、表示・全文同梱・ソース提供やライブラリ差し替えに関する確認が不要になるとは扱いません。

## 文書サイトのフォント

GitHub Pages の図の手書きに使う Yusei Magic（[SIL OFL 1.1](site/assets/YuseiMagic-OFL.txt)）は、図で使う文字だけに絞った WOFF を OFL の全文と一緒に `docs/site/assets/` に置いています。Reserved Font Name の指定がないので、同じ名前のまま改変できます。ページでは `Yusei Magic Subset` として読み込みます。

## 更新時の確認

自動で止まるもの: アプリの全文の欠け、`Package.resolved` や hosq の pin と notices のずれ（[test_licenses.py](../tests/python/test_licenses.py)）、VM イメージと `third-party/vm-boot/` のずれ（リリースの検査と `publish-vm.sh`）、ソースが未公開のディスク（リリースの検査）。VM の起動ディスクを変えたら `macOS/tools/vm-inventory.py` で生成し直します。

人が見るもの: Dawn の pin を変えたときの `third_party/` の内訳と、`web/package-lock.json` を更新したときの notices の版です。Tree-sitter のコピーしたライセンス全文も上流の同じ版と照合します。公開する ZIP を最後に確認し、ソース上の一覧を更新しただけで配布対応を完了としないようにします。
