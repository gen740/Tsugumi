# 固定しているバージョン

依存と実行環境の固定値をまとめています。アプリの導入・ビルド手順は [INSTALL.md](../INSTALL.md) を参照してください。

| 対象 | 版 |
| --- | --- |
| 対応環境 | Apple silicon Mac, macOS 26 以降（検証: macOS 27.2, M3 Max） |
| 共通 VM | カーネル 6.12.28（`macOS/Container/kernel.nix`、SELinux なし）+ 読み取り専用 rootfs（erofs、`macOS/Container/vm.nix`） |
| VM イメージ生成用ビルダー | `docker.io/nixos/nix:2.31.2` を Apple container 0.6.0 で実行（開発時のビルドだけ。アプリの実行には使わない） |
| nixpkgs | `1c89083e451e891152365b799595cf7790a43325`（`flake.lock`、Card の既定 `tsugumi:default` も同じ版） |
| FlatBuffers | 25.12.19（C++/Python は nixpkgs、Swift は SwiftPM で同版を固定） |
| hosq（C++ JSON） | `CLI20-dev/hosq@38c3c8e`（flake input） |
| Dawn | `v20261023.194543`（`ui/renderer/CMakeLists.txt`、Metal backend） |
| SwiftTerm（ターミナル表示、MIT） | 1.11.2（以降の版はシェーダーのために Xcode の Metal Toolchain の追加取得が要るため） |
