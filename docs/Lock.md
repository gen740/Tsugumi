# 固定しているバージョン

再現性を保つために固定している実行環境と依存ライブラリのバージョン一覧です。アプリの導入・ビルド手順は [INSTALL.md](../INSTALL.md) を参照してください。

| 対象 | 固定バージョン / リビジョン |
| --- | --- |
| 対応環境 | Apple silicon Mac, macOS 26 以降（検証環境：macOS 27.2, M3 Max） |
| 共通 VM | Linux カーネル 6.12.28（`macOS/Container/kernel.nix`、SELinux 無効）+ 読み取り専用 rootfs（erofs、`macOS/Container/vm-boot.nix`、Nix の入った nixpkgs だけの起動ディスクで gen740/tsugumi-vm に公開）。Tsugumi 自身は VM の中で同じ flake.lock の nixpkgs からビルド（`macOS/Container/vm-system.nix`） |
| VM イメージ生成用ビルダー | `docker.io/nixos/nix:2.31.2` を Apple container 0.6.0 上で実行（開発時の起動ディスクのビルドのみ。アプリ実行時には不使用） |
| nixpkgs | `1c89083e451e891152365b799595cf7790a43325`（`flake.lock`、Card のデフォルト `tsugumi:default` も同一リビジョン） |
| FlatBuffers | 25.12.19（C++ / Python は nixpkgs、Swift は SwiftPM で同一バージョンを固定） |
| hosq（C++ JSON） | `CLI20-dev/hosq@38c3c8e`（flake input） |
| Dawn（WebGPU） | `v20261023.194543`（`ui/renderer/CMakeLists.txt`、Metal バックエンド） |
| SwiftTerm（ターミナル表示、MIT） | 1.11.2（以降のバージョンはシェーダーコンパイルに Xcode Metal Toolchain の追加取得が必要となるため固定） |
