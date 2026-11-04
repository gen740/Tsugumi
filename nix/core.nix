# tsugumid, tsugumi-worker and the installed Card SDK.
{ lib, clangStdenv, cmake, ninja, pkg-config, flatbuffers, sqlite, libgit2, nix, openssl, bash, coreutils, findutils, gnugrep
, gnused, gawk, gnutar, gzip, jq, bubblewrap, self, cardNixpkgs, hosq }:
let
  # Tools on PATH for Card bash steps (preRun, a bash run, postRun).
  cardTools = [ bash coreutils findutils gnugrep gnused gawk gnutar gzip jq ];
in
# Clang on Linux too (with GCC's libstdc++, as the Nix libraries linked here):
# GCC 16 did not finish compiling one of the core's files.
clangStdenv.mkDerivation {
  pname = "tsugumid";
  version = lib.fileContents ../VERSION;
  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [ ../VERSION ../CMakeLists.txt ../cmake ../core ../sdk ../protocol ../tests/cpp ];
  };
  nativeBuildInputs = [ cmake ninja pkg-config flatbuffers ];
  # The embedded Nix evaluator: the same Nix as the environment's daemon.
  buildInputs = [ flatbuffers sqlite libgit2 nix.libs.nix-expr nix.libs.nix-store nix.libs.nix-fetchers nix.libs.nix-util ]
    # SHA-256 on Linux (macOS has CommonCrypto).
    ++ lib.optionals clangStdenv.hostPlatform.isLinux [ openssl ];
  cmakeFlags = [
    "-DHOSQ_INCLUDE_DIR=${hosq}/include"
    "-DTSUGUMI_BUILD_TESTS=OFF"
    "-DTSUGUMI_CARD_NIXPKGS_REV=${cardNixpkgs.rev}"
    "-DTSUGUMI_CARD_NIXPKGS_NARHASH=${cardNixpkgs.narHash}"
    "-DTSUGUMI_BASH=${bash}/bin/bash"
    "-DTSUGUMI_CARD_PATH=${lib.makeBinPath cardTools}"
  ] ++ lib.optionals clangStdenv.hostPlatform.isLinux [ "-DTSUGUMI_BWRAP=${bubblewrap}/bin/bwrap" ];
  meta = {
    description = "Tsugumi core daemon and Card worker";
    mainProgram = "tsugumid";
    platforms = [ "aarch64-linux" "x86_64-linux" "aarch64-darwin" ];
  };
}
