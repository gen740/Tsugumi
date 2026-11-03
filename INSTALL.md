# インストールと開発用ビルド

## 配布用アプリを使う

対応環境は **Apple silicon Mac / macOS 26 以降**です。
配布用の `Tsugumi.app` には計算の実行環境とデモが同梱されており、Nix・コンパイラ・コンテナ環境などを個別にセットアップする必要はありません。

1. 入手した `Tsugumi.app` を「アプリケーション」フォルダに置いて開きます。
2. ホーム画面の「デモを見る」から、試したいデモの「コピーを開く」を選びます。
3. 初回の実行環境の準備が完了したら、画面上部の「実行」を押します。

初回の環境準備はアプリが自動で行い、画面に進行状況を表示します。Card の依存パッケージを初回取得する際にはインターネット接続が必要です。
Agent を使う場合は、設定の「API」で利用するモデルのプロバイダと API キーを設定します。

基本的な操作は [README](README.md#画面でたどる実験と検証) と[操作ガイド](docs/Usage.md)、外部モデルへ送信されるデータの範囲は[セキュリティと隔離](docs/security.md#モデルに送られるもの)を参照してください。

## ソースからビルドする（開発者向け）

以下は、Tsugumi 本体や実行基盤を開発・変更する場合の手順です。コマンドは特記のない限りリポジトリのルートで実行します。
依存ライブラリの固定バージョンは [docs/Lock.md](docs/Lock.md) にまとめています。

前提環境：Nix（flakes 有効）、Xcode または Command Line Tools。VM イメージをビルドするときのみ Apple container 0.6.0（`container system start` 済み）を使用します。

```bash
nix develop
```

```bash
cmake -S . -B build -G Ninja && ninja -C build
```

共通 VM のイメージ生成（`dist/tsugumi-vm/` に出力）。カーネルと nixpkgs だけのベースは [gen740/tsugumi-vm](https://github.com/gen740/tsugumi-vm) の公開版をダウンロードし、Tsugumi のレイヤー（tsugumid など、数 MB）だけをコンテナ内の Nix でビルドします（別途の Linux ホストやリモートビルダーは不要）。ベースを変えたときは `macOS/tools/publish-vm.sh` で公開します：

```bash
macOS/Container/build-image.sh --vm
```

ビルダーのリソースは環境変数 `TSUGUMI_BUILDER_CPUS`（デフォルト `8`）、`TSUGUMI_BUILDER_MEMORY`（デフォルト `16G`）、`TSUGUMI_BUILDER_VOLUME`（Nix キャッシュ用 volume、デフォルト `tsugumi-builder-nix`）で調整できます。

Dawn 描画層のビルド（初回のみ Dawn と依存ライブラリの取得・ビルドで数分かかります）：

```bash
ui/renderer/build.sh
```

アプリ本体のパッケージング（`dist/Tsugumi.app` を生成。描画 dylib・デモ・初回用 VM イメージを同梱し、ローカル実行用の ad-hoc 署名を付与）：

```bash
macOS/build-app.sh
```

```bash
open dist/Tsugumi.app
```

VM の起動には virtualization entitlement 付きのコード署名が必要なため、開発中も `macOS/build-app.sh` で生成した `dist/Tsugumi.app` を使用します（`swift run` で直接起動したバイナリには署名がなく、VM を起動できません）。テストスクリプトが VM を操作するための `tsugumi-vmctl` は `macOS/tools/build-vmctl.sh` でビルドします。

アプリアイコン `macOS/Resources/AppIcon.icns` は `docs/assets/tsugumi-icon.svg` から生成してコミットしています。SVG を更新した場合は `swift macOS/Resources/tools/make_icon.swift` で再生成します。

## ビルド後の確認

```bash
(cd tests/python && python -m pytest -q test_contract.py)
```

テストスイートの詳細、パフォーマンス計測結果、開発時の既知の問題については[開発資料](docs/Development.md)を参照してください。
