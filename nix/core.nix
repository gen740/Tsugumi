# tsugumid, tsugumi-worker and the installed Card evaluation sources.
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
  cacert,
  cardNixpkgs,
  hosq,
  testNixPathLock ? {
    entries = [ ];
  },
  nixfmt,
  git,
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
  src = import ./core-source.nix { inherit lib; };
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
      "-DTSUGUMI_FORMAT_NIX=${nixfmt}/bin/nixfmt"
    ]
    ++ [
      "-DTSUGUMI_TEMPLATE_GIT=${git}/bin/git"
      "-DTSUGUMI_FLATBUFFERS_SOURCE=${flatbuffers.src}"
      "-DHOSQ_INCLUDE_DIR=${hosq}/include"
      "-DTSUGUMI_BUILD_TESTS=OFF"
      "-DTSUGUMI_TEST_SUPPORT_DIR=${placeholder "testSupport"}"
      "-DTSUGUMI_CARD_NIXPKGS_REV=${cardNixpkgs.rev}"
      "-DTSUGUMI_CARD_NIXPKGS_NARHASH=${cardNixpkgs.narHash}"
      "-DTSUGUMI_CACERT=${cacert}/etc/ssl/certs/ca-bundle.crt"
      "-DTSUGUMI_BASH=${bash}/bin/bash"
    ]
    ++ lib.optionals clangStdenv.hostPlatform.isLinux [ "-DTSUGUMI_BWRAP=${bubblewrap}/bin/bwrap" ];
  # What the dev shell's own CMake configuration needs of the above.
  passthru.devEnv = {
    TSUGUMI_TEST_NIX_LOCK = builtins.toJSON testNixPathLock;
    TSUGUMI_FORMAT_NIX = "${nixfmt}/bin/nixfmt";
    TSUGUMI_FLATBUFFERS_SOURCE = "${flatbuffers.src}";
    TSUGUMI_CACERT = "${cacert}/etc/ssl/certs/ca-bundle.crt";
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
