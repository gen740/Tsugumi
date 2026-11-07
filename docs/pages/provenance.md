# 制作範囲と事前準備

Tsugumi は、Claude のハッカソンで制作を始めた研究ワークベンチです。
**開始前からあった自作コードは JSON parser の hosq だけです。**

最初の仕様は開始前からずーーっと温め続けていたものでした（[私にしかない深さ](origin.md)）。

この区分は作者の申告と Git 履歴に基づきます。コミット日時はリポジトリへの記録日時であり、その内容を作った日時そのものではありません。

## 開始前に用意していたもの

| 対象 | 内容と確認先 |
| --- | --- |
| [hosq](https://github.com/CLI20-dev/hosq) | 作者が以前から開発している C++ の JSON parser。Tsugumi の文書・設定の JSON 処理に使用しています。ハッカソンで新しく作った parser ではありません。採用する版は [flake.lock](https://github.com/gen740/Tsugumi/blob/main/flake.lock) で固定しています。 |
| 最初の仕様プロンプト | Card・Workflow、実行環境、通信契約、UI・Agent の境界などを事前に検討したものです。[最初のコミットの instruction.md](https://github.com/gen740/Tsugumi/blob/98f90945ed92477e6e65ff48f1f410271edaaf96/instruction.md) に全文が残っています。実装コードではありませんが、設計の事前準備に含めます。 |

## Tsugumi として制作したもの

事前準備と外部 OSS を除き、次のアプリ固有の実装をこのプロジェクトで制作しました。既存ライブラリの機能そのものを、自作の成果として数えているわけではありません。

| 範囲 | このプロジェクトで実装した部分 | 利用している主な外部技術 |
| --- | --- | --- |
| `core/`、`protocol/` | Card と Workflow の文書モデル、グラフ実行、RPC 契約、履歴・成果物の管理 | hosq、FlatBuffers、Nix、SQLite、libgit2 |
| `sdk/`、`demos/` | Card 用 SDK、worker との連携、デモのグラフと処理コード | 各言語の処理系、Nix のパッケージ、デモで読む外部データ |
| `macOS/`、`ui/renderer/` | Canvas、編集・検証 UI、描画層との接続、VM の起動とアプリへの統合 | SwiftUI、Virtualization.framework、Dawn、Tree-sitter、SwiftTerm、Swift Markdown、SwiftMath |
| `agent/` | モデル接続、ツール呼び出し、Draft の実装・実行支援、共通の行動契約 | 外部のモデル API。モデル自体は開発成果に含めません。 |
| `web/` | 同じ RPC 契約を使う Web クライアントとホスト側サーバー | React、marked、KaTeX、FlatBuffers |
| `nix/`、`macOS/Container/`、テスト・ビルドスクリプト | 環境の固定、隔離の設定、VM とアプリの組み立て、実行・UI・契約の検証 | nixpkgs、Linux、bubblewrap、既存のビルド・テストツール |

実装は、人間が仕様・判断・レビューを担い、AI エージェントと往復しながら進めました。開発中に AI を使って実装・修正したことも制作過程に含めています。

初期の進行は、たとえば次の履歴で確認できます。

- [最初の仕様プロンプト（2026-11-02 13:15 JST）](https://github.com/gen740/Tsugumi/commit/98f90945ed92477e6e65ff48f1f410271edaaf96)
- [protocol と core の初期実装](https://github.com/gen740/Tsugumi/commit/eaaa7eb)
- [C++ / Python SDK と Collatz デモ](https://github.com/gen740/Tsugumi/commit/cfcb711)
- [SwiftUI アプリと Agent](https://github.com/gen740/Tsugumi/commit/e8b14d5)
- [Web UI の追加](https://github.com/gen740/Tsugumi/commit/4024439)

これは事前準備とアプリ固有の実装を区別する文書です。大会の正式な開始・終了日時や提出コミットは、この履歴だけからは確定できません。今後の追加機能・修正まで含む `main` 全体を、そのままハッカソン提出時の成果とみなさず、提出時点を説明する際は対象のコミットまたはタグを指定します。

## OSS・生成物・データの境界

Tree-sitter の実行系と言語 grammar、Dawn、Swift の依存パッケージ、Web の依存パッケージ、Linux と Nix のパッケージは、それぞれの開発者による OSS です。コピーしてリポジトリ内に置いたものも、この区分は変わりません。

FlatBuffers のスキーマや FBS 用 grammar は Tsugumi の実装ですが、そこから生成した binding・parser の行数を、手で実装した量とは区別します。FBS parser に付属する Tree-sitter の補助ヘッダーも外部コードです。ファイル単位の区分は [grammar の説明](https://github.com/gen740/Tsugumi/blob/main/macOS/Sources/Grammars/README.md) と [ライセンス調査](licenseaudit.md) に記載しています。

`demos/casa/` の処理は HEASARC の Chandra 観測アーカイブからデータを取得し、Chandra X-ray Center の CIAO をソースからビルドして使います。デモの処理・表示を作ったことと、天体観測データを作成したことは別です。[取得先と hash を使う定義](https://github.com/gen740/Tsugumi/blob/main/demos/casa/definitions/load/default.nix)を参照してください。

Tsugumi 自身のコードのライセンスは [MIT](https://github.com/gen740/Tsugumi/blob/main/LICENSE) です。既存の自作ライブラリ hosq、外部コード、フォント、観測データを、このライセンスで一括して再ライセンスするものではありません。依存先の条件と、配布に向けて残っている対応は [THIRD_PARTY_NOTICES.md](third-party-notices.md) と [ライセンス調査](licenseaudit.md) を参照してください。
