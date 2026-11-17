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
  icu,
  curl,
  nix,
  openssl,
  bash,
  bubblewrap,
  cardNixpkgs,
  hosq,
  llvmPackages,
  ruff,
  nixfmt,
  shfmt,
  withFormatters ? true,
}:
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
      ../workerd
      ../util
      ../rpc
      ../tsugumid
      ../hostd/CMakeLists.txt
      ../hostd/cmake
      ../hostd/src
      ../hostd/private
      ../hostd/agent/src
      ../hostd/agent/private
      ../vm/guest/CMakeLists.txt
      ../vm/guest/src
      ../client/CMakeLists.txt
      ../client/cmake
      ../client/include
      ../client/src
      ../sdk
      (lib.fileset.fileFilter (file: file.hasExt "json") ../ui/shared/templates)
      # Documentation tooling does not affect the native runtime.
      ../protocol/CMakeLists.txt
      (lib.fileset.fileFilter (file: file.hasExt "fbs") ../protocol)
      ../hostd/agent/Shared/system-prompt.md
      ../hostd/agent/Shared/guide-prompt.md
      ../hostd/agent/Shared/demo-skill.md
      ../hostd/agent/Shared/helpers-skill.md
      ../hostd/agent/Shared/review-skill.md
      ../hostd/agent/Shared/rerun-compare-skill.md
      ../hostd/agent/Shared/agent.json
      ../hostd/agent/Shared/guide-script.json
      ../hostd/agent/Shared/guide-script.en.json
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
    openssl
    nix.libs.nix-expr
    nix.libs.nix-store
    nix.libs.nix-fetchers
    nix.libs.nix-flake
    nix.libs.nix-util
  ]
  ++ lib.optionals clangStdenv.hostPlatform.isLinux [ icu ];
  cmakeFlags =
    lib.optionals withFormatters [
      "-DTSUGUMI_FORMAT_CLANG=${llvmPackages.clang-unwrapped}/bin/clang-format"
      "-DTSUGUMI_FORMAT_RUFF=${ruff}/bin/ruff"
      "-DTSUGUMI_FORMAT_NIX=${nixfmt}/bin/nixfmt"
      "-DTSUGUMI_FORMAT_SH=${shfmt}/bin/shfmt"
    ]
    ++ [
      "-DTSUGUMI_FLATBUFFERS_SOURCE=${flatbuffers.src}"
      "-DHOSQ_INCLUDE_DIR=${hosq}/include"
      "-DTSUGUMI_BUILD_TESTS=OFF"
      "-DTSUGUMI_TEST_SUPPORT_DIR=${placeholder "testSupport"}"
      "-DTSUGUMI_CARD_NIXPKGS_REV=${cardNixpkgs.rev}"
      "-DTSUGUMI_CARD_NIXPKGS_NARHASH=${cardNixpkgs.narHash}"
      "-DTSUGUMI_BASH=${bash}/bin/bash"
    ]
    ++ lib.optionals clangStdenv.hostPlatform.isLinux [ "-DTSUGUMI_BWRAP=${bubblewrap}/bin/bwrap" ];
  # What the dev shell's own CMake configuration needs of the above.
  passthru.devEnv = {
    TSUGUMI_FORMAT_CLANG = "${llvmPackages.clang-unwrapped}/bin/clang-format";
    TSUGUMI_FORMAT_RUFF = "${ruff}/bin/ruff";
    TSUGUMI_FORMAT_NIX = "${nixfmt}/bin/nixfmt";
    TSUGUMI_FORMAT_SH = "${shfmt}/bin/shfmt";
    TSUGUMI_FLATBUFFERS_SOURCE = "${flatbuffers.src}";
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
