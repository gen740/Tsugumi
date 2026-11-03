# インストールと開発用ビルド

## 配布用アプリを使う

対応環境は **Apple silicon Mac / macOS 26 以降**です。
配布用の `Tsugumi.app` には、計算の実行環境とデモを同梱します。利用者が Nix、コンパイラ、Apple container を個別にセットアップする必要はありません。

1. 入手した `Tsugumi.app` を「アプリケーション」フォルダに置いて開きます。
2. ホームの「デモを見る」から、試したいデモの「コピーを開く」を選びます。
3. 初回の実行環境の準備が終わったら、画面上部の「実行」を押します。

初回の準備はアプリが行い、画面に進捗を表示します。Card の依存パッケージを取得する際はインターネット接続が必要です。
Agent を使う場合は、アプリ設定の「API」で利用するモデルの接続先と API キーを設定します。

操作は [README](README.md#画面でたどる実験と検証) と[操作ガイド](docs/Usage.md)、モデルに送られる情報は[セキュリティと隔離](docs/security.md#モデルに送られるもの)を参照してください。

## ソースからビルドする（開発者向け）

以下は、アプリを開発・変更する場合の手順です。コマンドは、特記のない限りリポジトリのルートで実行します。
依存の固定バージョンは [docs/Lock.md](docs/Lock.md) にまとめています。

前提: Nix（flakes 有効）、Xcode または Command Line Tools。VM イメージを作るときだけ Apple container 0.6.0（`container system start` 済み）を使います。

```bash
nix develop
```

```bash
cmake -S . -B build -G Ninja && ninja -C build
```

共通 VM のイメージ（カーネルと rootfs を `dist/tsugumi-vm/` に。コンテナ内で Nix がビルドするので Linux ホストやリモートビルダーは不要）:

```bash
macOS/Container/build-image.sh --vm
```

ビルダーは `TSUGUMI_BUILDER_CPUS`（既定 8）, `TSUGUMI_BUILDER_MEMORY`（既定 16G）, `TSUGUMI_BUILDER_VOLUME`（Nix キャッシュ volume、既定 `tsugumi-builder-nix`）で調整できます。`TSUGUMI_IMAGE_OUT=dist/tsugumi-env.tar` を付けると、アプリ同梱用の OCI アーカイブも残します。

Dawn 描画層（初回は Dawn と依存の取得・ビルドで数分）:

```bash
ui/renderer/build.sh
```

アプリ本体（`dist/Tsugumi.app`。描画 dylib・デモ・初回用イメージを同梱、ローカル用 ad-hoc 署名）:

```bash
macOS/build-app.sh
```

```bash
open dist/Tsugumi.app
```

VM を使うには virtualization の entitlement 付きの署名が必要なので、開発中も `macOS/build-app.sh` で作った `dist/Tsugumi.app` を使います（`swift run` の実行ファイルは署名がなく VM を起動できません）。テストが VM を操作する `tsugumi-vmctl` は `macOS/tools/build-vmctl.sh` で作ります。

アプリアイコン `macOS/Resources/AppIcon.icns` は `docs/assets/tsugumi-icon.svg` から生成してコミットしています。SVG を変えたら `swift macOS/Resources/tools/make_icon.swift` で作り直します。

## ビルド後の確認

```bash
(cd tests/python && python -m pytest -q test_contract.py)
```

テストの種類、計測結果、開発時の既知の問題は[開発資料](docs/Development.md)を参照してください。
