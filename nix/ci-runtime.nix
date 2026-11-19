# The isolated runner and production outputs used by CI consumers.
{
  lib,
  stdenv,
  runCommand,
  makeWrapper,
  flatbuffers,
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
  ruby,
  openssl,
  core,
}:
runCommand "tsugumi-ci-runtime"
  {
    nativeBuildInputs = [ makeWrapper ];
    meta.platforms = core.meta.platforms;
  }
  ''
    mkdir -p "$out/bin" "$out/share/tsugumi"
    for program in ${core}/bin/*; do
      ln -s "$program" "$out/bin/$(basename "$program")"
    done
    ln -s ${core}/share/tsugumi/sdk "$out/share/tsugumi/sdk"
    ln -s ${core}/share/tsugumi/protocol "$out/share/tsugumi/protocol"
    ln -s ${core.testSupport}/python "$out/share/tsugumi/python"
    ln -s ${core}/lib "$out/lib"
    ln -s ${core}/include "$out/include"
    mkdir -p "$out/libexec/tsugumi-tests"
    cp ${../tests/run-isolated.py} "$out/libexec/tsugumi-tests/run-isolated.py"
    cat > "$out/libexec/tsugumi-tests/run-command" <<'EOF'
    #!${bash}/bin/bash
    set -euo pipefail
    exec python3 "$(dirname "$0")/run-isolated.py" "$@"
    EOF
    chmod +x "$out/libexec/tsugumi-tests/run-command"
    makeWrapper "$out/libexec/tsugumi-tests/run-command" "$out/bin/tsugumi-test-env" \
      --prefix PATH : ${
        lib.makeBinPath [
          (python3.withPackages (ps: [
            ps.pytest
            ps.pytest-xdist
            ps.flatbuffers
            ps.websockets
          ]))
          flatbuffers
          # Interpreter adapter tests must use the SDK's Ruby, not macOS Ruby 2.6.
          ruby
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
      --set TSUGUMI_HOSTD ${core}/bin/tsugumi-hostd \
      ${lib.optionalString stdenv.hostPlatform.isLinux "--set TSUGUMI_REMOTE_TEST_RAW_WORKER ${core}/bin/tsugumi-workerd"} \
      --set TSUGUMI_WORKER ${core}/bin/tsugumi-worker \
      --set TSUGUMI_WORKERD ${core}/bin/tsugumi-workerd \
      --set TSUGUMI_RPC_CLIENT ${core}/bin/tsugumi-rpc \
      --set TSUGUMI_SDK_DIR ${core}/share/tsugumi/sdk \
      --set TSUGUMI_AGENT_SCHEMA ${core}/share/tsugumi/protocol/agent.bfbs \
      --set TSUGUMI_PROJECT_SCHEMA ${core}/share/tsugumi/protocol/tsugumi.bfbs \
      --set TSUGUMI_PY_BINDINGS ${core.testSupport}/python \
      --set TSUGUMI_CLIENT_LIB_DIR ${core}/lib \
      --set TSUGUMI_FLATC ${flatbuffers}/bin/flatc
  ''
