# インストールと開発用ビルド

## 入手のしかた

Tsugumi はソースからビルドして使います。Apple の Developer ID による署名と公証はしていないため、ビルド済みのアプリは配布していません。下の手順で作った `dist/Tsugumi.app` は、そのマシンでだけ使える ad-hoc 署名（VM を動かすための virtualization entitlement つき）で、ほかの Mac へコピーすると Gatekeeper に止められます。

対応環境は **Apple silicon Mac / macOS 26 以降**です。初回の実行環境の準備（VM の中での Tsugumi の組み立てと、Card の依存パッケージの取得）にはインターネット接続が必要です。

最小の手順（詳細は次の節）:

```bash
ui/renderer/build.sh
```

```bash
macOS/build-app.sh
```

```bash
open dist/Tsugumi.app
```

ホーム画面の「デモを見る」から、試したいデモの「コピーを開く」を選び、初回の準備が終わったら画面上部の「実行」を押します。Agent を使う場合は、設定の「API」で利用するモデルのプロバイダと API キーを設定します。基本的な操作は [README](README.md#画面でたどる実験と検証) と[操作ガイド](docs/Usage.md)、外部モデルへ送信されるデータの範囲は[セキュリティと隔離](docs/security.md#モデルに送られるもの)を参照してください。

## ソースからビルドする

以下はアプリを作る手順と、Tsugumi 本体や実行基盤を開発・変更する場合の手順です。コマンドは特記のない限りリポジトリのルートで実行します。
依存ライブラリの固定バージョンは [docs/Lock.md](docs/Lock.md) にまとめています。

前提環境：Nix（flakes 有効）、Xcode または Command Line Tools。VM の起動ディスクをビルドするとき（公開版がないとき）のみ Apple container 0.6.0（`container system start` 済み）を使用します。

```bash
nix develop
```

```bash
cmake -S . -B build -G Ninja && ninja -C build
```

共通 VM の起動ディスク（カーネルと Nix の入った小さな rootfs、`dist/tsugumi-vm/` に出力）。[gen740/tsugumi-vm](https://github.com/gen740/tsugumi-vm) に公開版があればダウンロードし、なければコンテナ内の Nix でビルドします（別途の Linux ホストやリモートビルダーは不要）。変わるのは nixpkgs の固定か `macOS/Container/vm-boot.nix` を変えたときだけで、変えたら `macOS/tools/publish-vm.sh` で公開します。Tsugumi 自身（tsugumid など）は VM が初回起動時に、共有されたソースから VM の中の Nix でビルドします（`macOS/Container/vm-system.nix`。ソースが変わったときだけ作り直す）：

```bash
macOS/Container/build-image.sh --vm
```

ビルダーのリソースは環境変数 `TSUGUMI_BUILDER_CPUS`（デフォルト `8`）、`TSUGUMI_BUILDER_MEMORY`（デフォルト `16G`）、`TSUGUMI_BUILDER_VOLUME`（Nix キャッシュ用 volume、デフォルト `tsugumi-builder-nix`）で調整できます。

Dawn 描画層のビルド（初回のみ Dawn と依存ライブラリの取得・ビルドで数分かかります）：

```bash
ui/renderer/build.sh
```

アプリ本体のパッケージング（`dist/Tsugumi.app` を生成。描画 dylib・デモ・VM の起動ディスクと、VM が Tsugumi を組み立てるためのソース（`macOS/Container/vm-source.txt`）を同梱し、このマシン用の ad-hoc 署名を付与。起動ディスクがなければ `build-image.sh --vm` を先に実行します）：

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
