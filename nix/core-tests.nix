{
  lib,
  clangStdenv,
  cmake,
  ninja,
  pkg-config,
  makeWrapper,
  flatbuffers,
  sqlite,
  libgit2,
  icu,
  curl,
  gtest,
  openssl,
  nix,
  core,
  hosq,
  runtime,
  mock,
  inputTypes,
  # The narrow consumers configure only the target they actually execute.
  target ? null,
  nativeDarwin ? false,
}:
clangStdenv.mkDerivation {
  pname =
    if nativeDarwin then
      "tsugumi-native-darwin-tests"
    else if target == null then
      "tsugumi-core-tests"
    else
      target;
  inherit (core) version;
  src = lib.fileset.toSource {
    root = ../.;
    # Python cases run from the checkout. Editing a scenario does not rebuild
    # the production core or any cached C++ test executable.
    fileset = lib.fileset.unions (
      if target == null then
        [
          ../hostd/tools/test-binary-cache.py
          ../vm/guest/src/control_wire.hh
          (lib.fileset.fileFilter (
            file: file.name == "CMakeLists.txt" || file.hasExt "cmake" || file.hasExt "cc" || file.hasExt "hh"
          ) ../tests/core)
        ]
        ++ lib.optionals (!nativeDarwin) [
          ../hostd/agent/Shared/guide-script.json
          ../hostd/agent/Shared/guide-script.en.json
          ../demos
          ../tests/fixtures/layout-invalid.json
          ../tests/fixtures/layout-scenes.json
          ../tests/fixtures/wire-routes.json
        ]
      else
        [
          ../tests/core/CMakeLists.txt
          ../tests/core/cmake/installed-core.cmake
          ../tests/core/cmake/installed
        ]
        ++ {
          tsugumid-mock = [
            ../tests/core/cmake/daemon-double.cmake
            ../tests/core/doubles/nix_eval.cc
          ];
          tsugumi_input_types_tests = [
            ../tests/core/cmake/input-types.cmake
            ../tests/core/doubles/nix_eval.cc
            ../tests/core/input_types_test.cc
          ];
          tsugumi_document_edit_driver = [
            ../tests/core/cmake/host-edit-driver.cmake
            ../tests/core/agent/document_edit_driver.cc
          ];
          tsugumi_arm_shell_tests = [
            ../tests/core/cmake/host-rpc.cmake
            ../tests/core/cmake/host-rpc-host-sources.cmake
            ../tests/core/cmake/host-rpc-environment-sources.cmake
            ../tests/core/cmake/host-rpc-settings-sources.cmake
            ../tests/core/cmake/host-rpc-versions-sources.cmake
            ../tests/core/agent/project_rpc_test.cc
            (lib.fileset.fileFilter (file: file.hasExt "hh") ../tests/core/agent)
            ../tests/core/agent_fixture_server.cc
            ../tests/core/agent_fixture_server.hh
          ];
          tsugumi_vm_lifetime_tests = [
            ../tests/core/cmake/vm.cmake
            ../tests/core/vm_lifetime_test.cc
            ../tests/core/vm_guest_wire_test.cc
            ../vm/guest/src/control_wire.hh
          ];
        }
        .${target}
    );
  };
  cmakeDir = "../tests/core";
  nativeBuildInputs = [
    cmake
    ninja
    pkg-config
  ]
  ++ lib.optional (target == null) makeWrapper;
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
  ++ lib.optionals clangStdenv.hostPlatform.isLinux [ icu ]
  ++ lib.optional (target != "tsugumid-mock" && target != "tsugumi_document_edit_driver") gtest;
  cmakeFlags = [
    "-DTSUGUMI_CORE_ROOT=${core}"
    "-DTSUGUMI_CORE_TEST_SUPPORT=${core.testSupport}"
    "-DHOSQ_INCLUDE_DIR=${hosq}/include"
  ]
  ++ lib.optional nativeDarwin "-DTSUGUMI_NATIVE_DARWIN_TESTS=ON"
  ++ lib.optional (
    target == "tsugumi_arm_shell_tests"
  ) "-DTSUGUMI_TEST_MOCK=${mock}/libexec/tsugumi-tests/tsugumid-mock"
  ++ (
    if target == null then
      [
        "-DTSUGUMI_TEST_MOCK=${mock}/libexec/tsugumi-tests/tsugumid-mock"
      ]
    else
      [
        "-DTSUGUMI_TEST_TARGET=${target}"
      ]
  );
  # RPC listeners, process failures and host isolation are tested on the
  # runner after caching this output, not inside the Nix build sandbox.
  doCheck = false;
  doInstallCheck = false;
  postInstall = lib.optionalString (target == null) ''
    mkdir -p "$out/bin" "$out/share/tsugumi"
    for program in ${core}/bin/*; do
      ln -s "$program" "$out/bin/$(basename "$program")"
    done
    ln -s ${runtime}/share/tsugumi/* "$out/share/tsugumi/"
    ln -s ${runtime}/{lib,include} "$out/"
    ln -s ${mock}/libexec/tsugumi-tests/tsugumid-mock "$out/libexec/tsugumi-tests/"
    ${lib.optionalString (!nativeDarwin) ''
      ln -s ${inputTypes}/libexec/tsugumi-tests/tsugumi_input_types_tests "$out/libexec/tsugumi-tests/"
    ''}
    makeWrapper ${runtime}/bin/tsugumi-test-env "$out/bin/tsugumi-test-env" \
      --set TSUGUMI_MOCK_DAEMON "$out/libexec/tsugumi-tests/tsugumid-mock" \
      ${lib.optionalString (
        !nativeDarwin
      ) ''--set TSUGUMI_INPUT_TYPES_TESTS "$out/libexec/tsugumi-tests/tsugumi_input_types_tests"''}
  '';
  meta = {
    description = "Reusable public RPC test binaries and runner environment for Tsugumi";
    platforms = core.meta.platforms;
  };
}
