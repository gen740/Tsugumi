# The Linux desktop UI (ui/linux, GTK 4). It configures the same CMake tree
# as the core but compiles only its own targets and what they link (the
# client, the shared hostd launch and JSON), and runs the host the core
# package installs.
{
  lib,
  clangStdenv,
  cmake,
  ninja,
  pkg-config,
  wrapGAppsHook4,
  flatbuffers,
  sqlite,
  libgit2,
  icu,
  curl,
  nix,
  openssl,
  bash,
  bubblewrap,
  gtk4,
  git,
  cardNixpkgs,
  hosq,
  core,
  web,
}:
clangStdenv.mkDerivation {
  pname = "tsugumi-gtk";
  version = lib.fileContents ../VERSION;
  outputs = [
    "out"
    "tests"
  ];
  src = import ./core-source.nix {
    inherit lib;
    extra = [
      ../ui/linux
      ../ui/shared/i18n/en.json
    ];
  };
  nativeBuildInputs = [
    cmake
    ninja
    pkg-config
    flatbuffers
    wrapGAppsHook4
  ];
  # The core's configuration needs its libraries; none of its sources build.
  buildInputs = [
    flatbuffers
    sqlite
    libgit2
    curl
    openssl
    icu
    gtk4
    nix.libs.nix-expr
    nix.libs.nix-store
    nix.libs.nix-fetchers
    nix.libs.nix-flake
    nix.libs.nix-util
  ];
  cmakeFlags = [
    "-DTSUGUMI_BUILD_LINUX_UI=ON"
    "-DTSUGUMI_BUILD_TESTS=OFF"
    "-DTSUGUMI_TEMPLATE_GIT=${git}/bin/git"
    "-DTSUGUMI_FLATBUFFERS_SOURCE=${flatbuffers.src}"
    "-DHOSQ_INCLUDE_DIR=${hosq}/include"
    "-DTSUGUMI_CARD_NIXPKGS_REV=${cardNixpkgs.rev}"
    "-DTSUGUMI_CARD_NIXPKGS_NARHASH=${cardNixpkgs.narHash}"
    "-DTSUGUMI_BASH=${bash}/bin/bash"
    "-DTSUGUMI_BWRAP=${bubblewrap}/bin/bwrap"
  ];
  ninjaFlags = [
    "tsugumi-gtk"
    "tsugumi_linux_ui_tests"
    "tsugumi_gtk_smoke_test"
  ];
  doCheck = true;
  checkPhase = ''
    runHook preCheck
    ui/linux/tsugumi_linux_ui_tests
    runHook postCheck
  '';
  installPhase = ''
    runHook preInstall
    install -Dm755 ui/linux/tsugumi-gtk $out/bin/tsugumi-gtk
    install -Dm644 $src/ui/linux/dev.tsugumi.Tsugumi.desktop $out/share/applications/dev.tsugumi.Tsugumi.desktop
    install -Dm755 ui/linux/tsugumi_gtk_smoke_test $tests/libexec/tsugumi-gtk/tsugumi_gtk_smoke_test
    runHook postInstall
  '';
  # The host and project runtime this UI starts (as nix/cli.nix gives the
  # tsugumi command); the bundled demos and Web UI come from the Web output.
  preFixup = ''
    gappsWrapperArgs+=(
      --set-default TSUGUMI_REPO ${web}/share/tsugumi
      --set-default TSUGUMI_HOSTD ${core}/bin/tsugumi-hostd
      --set-default TSUGUMID ${core}/bin/tsugumid
      --set-default TSUGUMI_WORKER ${core}/bin/tsugumi-worker
      --set-default TSUGUMI_SDK_DIR ${core}/share/tsugumi/sdk
    )
  '';
  meta = {
    description = "Tsugumi's desktop UI for Linux (GTK 4)";
    mainProgram = "tsugumi-gtk";
    platforms = [
      "x86_64-linux"
      "aarch64-linux"
    ];
  };
}
