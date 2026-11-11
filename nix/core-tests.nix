{
  lib,
  clangStdenv,
  cmake,
  ninja,
  pkg-config,
  makeWrapper,
  flatbuffers,
  sqlite,
  postgresql,
  libgit2,
  curl,
  gtest,
  openssl,
  bash,
  coreutils,
  git,
  nodejs,
  jq,
  nix,
  clang-tools,
  ruff,
  nixfmt,
  shfmt,
  python3,
  core,
  hosq,
}:
clangStdenv.mkDerivation {
  pname = "tsugumi-core-tests";
  inherit (core) version;
  src = lib.fileset.toSource {
    root = ../.;
    # Python cases run from the checkout. Editing a scenario does not rebuild
    # the production core or any cached C++ test executable.
    fileset = lib.fileset.fileFilter (
      file: file.name == "CMakeLists.txt" || file.hasExt "cmake" || file.hasExt "cc" || file.hasExt "hh"
    ) ../tests/core;
  };
  cmakeDir = "../tests/core";
  nativeBuildInputs = [
    cmake
    ninja
    pkg-config
    makeWrapper
  ];
  buildInputs = [
    flatbuffers
    sqlite
    postgresql
    libgit2
    curl
    gtest
  ]
  ++ lib.optionals clangStdenv.hostPlatform.isLinux [ openssl ];
  cmakeFlags = [
    "-DTSUGUMI_CORE_ROOT=${core}"
    "-DTSUGUMI_CORE_TEST_SUPPORT=${core.testSupport}"
    "-DHOSQ_INCLUDE_DIR=${hosq}/include"
  ];
  # RPC listeners, process failures and host isolation are tested on the
  # runner after caching this output, not inside the Nix build sandbox.
  doCheck = false;
  doInstallCheck = false;
  postInstall = ''
    mkdir -p "$out/bin" "$out/share/tsugumi"
    for program in ${core}/bin/*; do
      ln -s "$program" "$out/bin/$(basename "$program")"
    done
    ln -s ${core}/share/tsugumi/sdk "$out/share/tsugumi/sdk"
    ln -s ${core}/share/tsugumi/protocol "$out/share/tsugumi/protocol"
    ln -s ${core.testSupport}/python "$out/share/tsugumi/python"
    ln -s ${core}/lib "$out/lib"
    ln -s ${core}/include "$out/include"
    cat > "$out/libexec/tsugumi-tests/run-command" <<'EOF'
    #!${bash}/bin/bash
    set -euo pipefail
    if [ "$#" -eq 0 ]; then
      echo 'Usage: tsugumi-test-env COMMAND [ARGUMENT...]' >&2
      exit 2
    fi
    exec "$@"
    EOF
    chmod +x "$out/libexec/tsugumi-tests/run-command"
    makeWrapper "$out/libexec/tsugumi-tests/run-command" "$out/bin/tsugumi-test-env" \
      --prefix PATH : ${
        lib.makeBinPath [
          (python3.withPackages (ps: [
            ps.pytest
            ps.flatbuffers
            ps.websockets
          ]))
          flatbuffers
          postgresql
          bash
          coreutils
          openssl
          git
          nodejs
          jq
          nix
          clang-tools
          ruff
          nixfmt
          shfmt
        ]
      } \
      --set TSUGUMID ${core}/bin/tsugumid \
      --set TSUGUMI_AGENTD ${core}/bin/tsugumi-agentd \
      --set TSUGUMI_WORKER ${core}/bin/tsugumi-worker \
      --set TSUGUMI_RPC_CLIENT ${core}/bin/tsugumi-rpc \
      --set TSUGUMI_MOCK_DAEMON "$out/libexec/tsugumi-tests/tsugumid-mock" \
      --set TSUGUMI_SDK_DIR ${core}/share/tsugumi/sdk \
      --set TSUGUMI_AGENT_SCHEMA ${core}/share/tsugumi/protocol/agent.bfbs \
      --set TSUGUMI_PROJECT_SCHEMA ${core}/share/tsugumi/protocol/tsugumi.bfbs \
      --set TSUGUMI_PY_BINDINGS ${core.testSupport}/python \
      --set TSUGUMI_CLIENT_LIB_DIR ${core}/lib \
      --set TSUGUMI_FLATC ${flatbuffers}/bin/flatc
  '';
  meta = {
    description = "Reusable public RPC test binaries and runner environment for Tsugumi";
    platforms = core.meta.platforms;
  };
}
