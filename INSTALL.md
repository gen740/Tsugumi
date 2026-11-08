# インストールと開発用ビルド

## 入手のしかた

**macOS・Linux・Windows に対応しています**（macOS と Linux（Ubuntu・NixOS）で確認。Windows は WSL2 で検証済み）。デスクトップアプリは Apple silicon Mac / macOS 26 以降だけで、Linux と Windows では [`tsugumi` コマンド](#ブラウザで使うtsugumi-コマンド)の Web UI を使います。

初回の実行環境の準備（VM の中での Tsugumi の組み立てと、Card の依存パッケージの取得）にはインターネット接続が必要です。

### デスクトップアプリ（Mac）を入れる

[GitHub Releases](https://github.com/gen740/Tsugumi/releases) の最新版を `/Applications` に入れます。

`Tsugumi.dmg` を開いて `Applications` へドラッグするか、次のコマンドで入れます。

```bash
curl -fL -o /tmp/Tsugumi.zip https://github.com/gen740/Tsugumi/releases/latest/download/Tsugumi.zip && ditto -x -k /tmp/Tsugumi.zip /Applications && open /Applications/Tsugumi.app
```

リリース版は Apple の Developer ID による署名と公証をしていない ad-hoc 署名（VM を動かすための virtualization entitlement つき）です。ブラウザでダウンロードした zip から取り出したアプリは Gatekeeper に止められるので、上のように `curl` で取得するか、取り出したあとに隔離属性を外してください。

```bash
xattr -dr com.apple.quarantine /Applications/Tsugumi.app
```

ホーム画面の「デモを見る」から、試したいデモの「コピーを開く」を選び、初回の準備が終わったら画面上部の「実行」を押します。Agent を使う場合は、設定の「API」で利用するモデルのプロバイダと API キーを設定するか、ChatGPT プランを選んで「Continue with ChatGPT」でサインインします。基本的な操作は [README](README.md#画面でたどる実験と検証) と[操作ガイド](docs/Usage.md)、外部モデルへ送信されるデータの範囲は[セキュリティと隔離](docs/security.md#モデルに送られるもの)にあります。

### 自動更新

リリース版は起動後と 6 時間ごとに GitHub の最新リリースを確認し、新しい版があればダウンロードして、チェックサム・署名・bundle id・版番号を確かめてから、**次に終了したとき**に入れ替えます（終了時には VM が止まっているので、実行中の計算が更新で中断されることはありません）。すぐに更新したいときは、ホーム画面の「再起動して更新」か、アプリケーションメニューの「再起動して … に更新」を選びます。設定の「一般」→「アップデート」で、今の版の確認、手動での確認、自動確認のオン・オフができます。

- アプリを置いたフォルダ（通常は `/Applications`）に書き込めることが必要です。隔離属性がついたまま開いたアプリ（App Translocation で別の場所から動いている）は更新できないので、`/Applications` に移して隔離属性を外してください。
- ad-hoc 署名は版ごとに変わるため、更新後に初めて API キーを使うとき、macOS がキーチェーンへのアクセスを一度確認します（「常に許可」を選べば、その版では以後確認されません）。
- 安定版は v1.0.0 からです。いまの 0.x の版の間では、プロジェクトの文書や設定の互換性を保たず、移行もしません（[AGENTS.md](AGENTS.md)）。

### ブラウザで使う（tsugumi コマンド）

アプリの代わりに、ブラウザの [Web UI](docs/WebUI.md) で使うコマンドです。root や管理者権限は要りません。

Mac・Linux:

```bash
curl -fsSL https://github.com/gen740/Tsugumi/releases/latest/download/install.sh | sh
```

Windows（PowerShell）:

```powershell
irm https://github.com/gen740/Tsugumi/releases/latest/download/install.ps1 | iex
```

`~/.local/bin/tsugumi` が入ります。実行するとサーバーが起動してブラウザが開きます。

```bash
tsugumi
```

止めるときは Ctrl-C です。すでに動いていれば、新しいページを開くだけです。`tsugumi <プロジェクトのフォルダ>` でそのプロジェクトを開きます。

新しい版が出たら、次のコマンドで入れ替えます（すでに最新ならそう伝えて、何もしません）。動いている `tsugumi` は、止めて起動し直すと新しい版で開きます。

```bash
tsugumi update
```

- **入るもの**: コマンド（`~/.local/bin/tsugumi`）と、その中身（`~/.local/share/tsugumi/<版>/`）だけです。`~/.local/bin` が `PATH` にないときはそう表示し、シェルの設定は変更しません。`tsugumi update` か入れ直しで新しい版に置き換わります（自動では更新しません）。消すときは `rm ~/.local/bin/tsugumi` と `rm -r ~/.local/share/tsugumi` です。
- **Mac（Apple silicon / macOS 26 以降）**: Node.js と VM の起動ディスクを含む `tsugumi-macos-arm64.tar.gz` を取得します。Card はデスクトップ版と同じ共通 Linux VM で動くので、Nix を別に入れる必要はありません。初回は VM の中で Tsugumi を組み立てるので数分かかります。デスクトップ版と共有する設定や、同時に使えない場合は [Web UI](docs/WebUI.md#tsugumi-コマンド) にあります。
- **Linux（x86_64 / aarch64）**: [Nix](https://nixos.org/download) が必要です。ソースの `tsugumi-linux.tar.gz` を取得して Nix でビルドします（初回は数分かかります。動き方は [Web UI](docs/WebUI.md#tsugumi-コマンド)）。
- **Windows**: WSL2 と、その中の Linux に入れた [Nix](https://nixos.org/download) が必要です。`install.ps1` は既定の WSL の Linux で `install.sh` を実行し、`%USERPROFILE%\.local\bin\tsugumi.cmd` を作ります（`PATH` は変更しません）。`tsugumi` は WSL の中で Linux と同じように動き、ページは Windows のブラウザに開きます。プロジェクトは WSL のファイルシステムに置き、フォルダは WSL から見た絶対パスで渡します。WSL2（Nix は daemon なし）に入れて、デモを開き、実行するところまで検証しました（[#159](https://github.com/gen740/Tsugumi/issues/159)）。WSL の Linux のシェルで、Linux と同じ `curl … | sh` で入れることもできます（`tsugumi.cmd` は作られませんが、ページは同じく Windows のブラウザに開きます）。
- 取得したファイルはチェックサム（`.sha256`）を確かめてから入れます。版を指定するときは環境変数 `TSUGUMI_VERSION` を使います。

## ソースからビルドする

以下はアプリを作る手順と、Tsugumi 本体や実行基盤を開発・変更する場合の手順です。コマンドは特記のない限りリポジトリのルートで実行します。

ソースからビルドしたアプリは**開発版**（Tsugumi Dev）です。リリース版とは別のアプリとしてサポートフォルダ（`~/Library/Application Support/Tsugumi-dev/`）を分けるので、リリース版の VM に触れず、両方を同時に起動できます。開発版は自動更新しません（[開発資料](docs/Development.md#リリース)の「開発版とリリース版」）。

### 前提環境

- Nix（flakes 有効）
- Xcode または Command Line Tools
- VM の起動ディスクをビルドするとき（[公開版](https://github.com/gen740/tsugumi-vm)がないとき）のみ、Apple container（版は [macOS/Container/versions.env](macOS/Container/versions.env) の `TSUGUMI_CONTAINER_VERSION`。`container system start` 済み）

依存ライブラリの固定バージョンは [docs/Lock.md](docs/Lock.md) にまとめています。

### 最小の手順

```bash
ui/renderer/build.sh
```

```bash
macOS/build-app.sh
```

```bash
open dist/Tsugumi.app
```

### 各段階の詳細

開発シェルに入り、core（tsugumid）と SDK をビルドします：

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

ビルダーのリソースは環境変数 `TSUGUMI_BUILDER_CPUS`（既定 `8`）、`TSUGUMI_BUILDER_MEMORY`（既定 `16G`）、`TSUGUMI_BUILDER_VOLUME`（Nix キャッシュ用 volume、既定 `tsugumi-builder-nix`）で調整できます。

[最小の手順](#最小の手順)の `ui/renderer/build.sh` は Dawn 描画層をビルドします（初回のみ Dawn と依存ライブラリの取得・ビルドで数分かかります）。

`macOS/build-app.sh` はアプリ本体をパッケージングし、`dist/Tsugumi.app` を生成します。描画 dylib・デモ・VM の起動ディスクと、VM が Tsugumi を組み立てるためのソース（`macOS/Container/vm-source.txt`）を同梱し、このマシン用の ad-hoc 署名を付与します。起動ディスクがなければ `build-image.sh --vm` を先に実行します。`--release` をつけるとリリース版として作ります（リリースの workflow が使う。[開発資料](docs/Development.md#リリース)）。

VM の起動には virtualization entitlement 付きのコード署名が必要なため、開発中も `macOS/build-app.sh` で生成した `dist/Tsugumi.app` を使用します（`swift run` で直接起動したバイナリには署名がなく、VM を起動できません）。テストスクリプトが VM を操作するための `tsugumi-vmctl` は `macOS/tools/build-vmctl.sh` でビルドします。

アプリアイコン `macOS/Resources/AppIcon.icns` は `docs/assets/tsugumi-icon.svg` から生成してコミットしています。SVG を更新した場合は `swift macOS/Resources/tools/make_icon.swift` で再生成します。

## ビルド後の確認

```bash
(cd tests/python && python -m pytest -q test_contract.py)
```

テストスイートと確認範囲は[開発資料のテスト](docs/Development.md#テスト)にあります。
