# tsugumid, tsugumi-worker and the installed Card SDK.
{
  lib,
  clangStdenv,
  cmake,
  ninja,
  pkg-config,
  flatbuffers,
  sqlite,
  libgit2,
  curl,
  nix,
  openssl,
  bash,
  coreutils,
  findutils,
  gnugrep,
  gnused,
  gawk,
  gnutar,
  gzip,
  jq,
  bubblewrap,
  self,
  cardNixpkgs,
  hosq,
}:
let
  # Tools on PATH for Card bash steps (preRun, a bash run, postRun).
  cardTools = [
    bash
    coreutils
    findutils
    gnugrep
    gnused
    gawk
    gnutar
    gzip
    jq
  ];
in
# Clang on Linux too (with GCC's libstdc++, as the Nix libraries linked here):
# GCC 16 did not finish compiling one of the core's files.
clangStdenv.mkDerivation {
  pname = "tsugumid";
  version = lib.fileContents ../VERSION;
  # The CI tests link the same compiled core without rebuilding its sources.
  # Production callers only retain `out`; private headers stay in testSupport.
  outputs = [
    "out"
    "testSupport"
  ];
  # CMake deliberately separates public client headers from private test inputs.
  moveToDev = false;
  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [
      ../VERSION
      ../CMakeLists.txt
      ../cmake
      ../core
      ../client
      ../sdk
      # Documentation tooling does not affect the native runtime.
      (lib.fileset.fileFilter (file: file.hasExt "fbs") ../protocol)
      ../agent/Shared/system-prompt.md
      ../agent/Shared/guide-prompt.md
      ../agent/Shared/demo-skill.md
    ];
  };
  nativeBuildInputs = [
    cmake
    ninja
    pkg-config
    flatbuffers
  ];
  # The embedded Nix evaluator: the same Nix as the environment's daemon.
  buildInputs = [
    flatbuffers
    sqlite
    libgit2
    curl
    nix.libs.nix-expr
    nix.libs.nix-store
    nix.libs.nix-fetchers
    nix.libs.nix-flake
    nix.libs.nix-util
  ]
  # SHA-256 on Linux (macOS has CommonCrypto).
  ++ lib.optionals clangStdenv.hostPlatform.isLinux [ openssl ];
  cmakeFlags = [
    "-DHOSQ_INCLUDE_DIR=${hosq}/include"
    "-DTSUGUMI_BUILD_TESTS=OFF"
    "-DTSUGUMI_TEST_SUPPORT_DIR=${placeholder "testSupport"}"
    "-DTSUGUMI_CARD_NIXPKGS_REV=${cardNixpkgs.rev}"
    "-DTSUGUMI_CARD_NIXPKGS_NARHASH=${cardNixpkgs.narHash}"
    "-DTSUGUMI_BASH=${bash}/bin/bash"
    "-DTSUGUMI_CARD_PATH=${lib.makeBinPath cardTools}"
  ]
  ++ lib.optionals clangStdenv.hostPlatform.isLinux [ "-DTSUGUMI_BWRAP=${bubblewrap}/bin/bwrap" ];
  # What the dev shell's own CMake configuration needs of the above.
  passthru.devEnv = {
    HOSQ_INCLUDE_DIR = "${hosq}/include";
    TSUGUMI_CARD_NIXPKGS_REV = cardNixpkgs.rev;
    TSUGUMI_CARD_NIXPKGS_NARHASH = cardNixpkgs.narHash;
  };
  meta = {
    description = "Tsugumi core daemon and Card worker";
    mainProgram = "tsugumid";
    platforms = [
      "aarch64-linux"
      "x86_64-linux"
      "aarch64-darwin"
    ];
  };
}
