# 固定しているバージョン

再現性を保つために固定している実行環境と依存ライブラリのバージョン一覧です。アプリの導入・ビルド手順は [INSTALL.md](install.md) を参照してください。

| 対象 | 固定バージョン / リビジョン |
| --- | --- |
| 対応環境 | macOS（Apple silicon, macOS 26 以降。検証環境：macOS 27.2, M3 Max）、Linux（x86_64・aarch64、Nix が必要。検証環境：OrbStack の VM（Ubuntu 26.04、aarch64）、友人の Ubuntu（x86_64）、NixOS（x86_64、SSH 越し））、Windows（WSL2 と Nix が必要。**未検証**） |
| Node.js（Mac の `tsugumi` コマンドに同梱） | `cli/node-version.env`（公式ビルドをチェックサムで確認）。Linux と Windows は固定した nixpkgs の `nodejs` |
| 共通 VM | Linux カーネル 6.12.115（`macOS/Container/kernel.nix`、SELinux・io_uring 無効、非特権 BPF 無効）+ 読み取り専用 rootfs（erofs、`macOS/Container/vm-boot.nix`、Nix の入った nixpkgs だけの起動ディスクで gen740/tsugumi-vm に公開）。Tsugumi 自身は VM の中で同じ flake.lock の nixpkgs からビルド（`macOS/Container/vm-system.nix`） |
| VM イメージ生成用ビルダー | `docker.io/nixos/nix:2.31.2` を Apple container 0.6.0 上で実行（開発時の起動ディスクのビルドのみ。アプリ実行時には不使用） |
| nixpkgs | `1c89083e451e891152365b799595cf7790a43325`（`flake.lock`、Card の既定 `tsugumi:default` も同一リビジョン） |
| FlatBuffers | 25.12.19（C++ / Python は nixpkgs、Swift は SwiftPM で同一バージョンを固定） |
| hosq（C++ JSON） | `CLI20-dev/hosq@1eb8942`（flake input） |
| Dawn（WebGPU） | `v20261023.194543`（`ui/renderer/CMakeLists.txt`、Metal バックエンド） |
| SwiftTerm（ターミナル表示、MIT） | 1.11.2（以降のバージョンはシェーダーコンパイルに Xcode Metal Toolchain の追加取得が必要となるため固定） |
| Swift Markdown / SwiftMath（チャット・付箋・Card の Markdown と数式） | 0.7.3 / 1.7.3（`macOS/Package.swift`） |
| SwiftTreeSitter と各言語の grammar（エディターの構文強調） | SwiftTreeSitter 0.25.0、grammar はそれぞれの版（`macOS/Package.swift`） |
| xterm.js / addon-fit / node-pty（Web UI のターミナル、MIT） | 5.5.0 / 0.10.0 / 1.1.0（`web/package-lock.json`） |
| libgit2（版の管理） | 1.9.7（固定した nixpkgs の `libgit2`） |
| plotly.js / Vega / Vega-Lite / Vega-Embed（Card のグラフの表示部品） | 3.6.0 / 6.4.0 / 6.4.3 / 7.3.0（`web/package-lock.json`。Mac アプリも同じファイルを `build-app.sh` でコピー） |
