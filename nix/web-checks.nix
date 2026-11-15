# Type and behavior checks consume the exact package build's dependencies.
# A test-only edit reruns this derivation without rebuilding the Web bundle.
{
  lib,
  runCommand,
  nodejs,
  cmake,
  clangStdenv,
  flatbuffers,
  hosq,
  openssl,
  web,
}:
runCommand "tsugumi-web-checks"
  {
    nativeBuildInputs = [
      nodejs
      cmake
      clangStdenv.cc
      flatbuffers
    ];
    testSources = lib.fileset.toSource {
      root = ../.;
      fileset = lib.fileset.unions [
        ../web/scripts/test.mjs
        ../web/tests
        ../core/src/host/document_edits.cc
        ../core/src/host/document_edits.hh
        ../core/src/host/templates.cc
        ../core/src/host/templates.hh
        ../cmake/embed_templates.cmake
        ../cmake/embed.cmake
        ../core/src/agent/json.hh
        ../core/src/schema.hh
        ../core/src/schema.cc
        ../core/src/util/crypto.cc
        ../core/src/util/crypto.hh
        ../tests/core/agent/document_edit_driver.cc
        ../tests/fixtures
        ../agent/Shared/system-prompt.md
        ../agent/Shared/mcp-transport.md
      ];
    };
  }
  ''
    export HOME="$TMPDIR"
    cp -R ${web.src} source
    chmod -R u+w source
    cp -R $testSources/. source/
    ln -s ${web.testSupport}/node_modules source/web/node_modules
    cp -R ${web.testSupport}/gen source/web/src/gen
    export TSUGUMI_SITE_PLAYER_DIR=${web.sitePlayer}
    # Gesture and undo tests call the host's pure document operations. Compile
    # only those sources here; no daemon, sockets or full core build is needed.
    cmake -DINPUT_DIR="$PWD/source/ui/templates" -DOUTPUT="$PWD/host_templates.cc" \
      -P source/cmake/embed_templates.cmake
    mkdir -p generated/tsugumi/protocol
    flatc --cpp --gen-object-api --gen-all -I source/protocol \
      -o generated/tsugumi/protocol source/protocol/host.fbs
    ${clangStdenv.cc}/bin/clang++ -std=c++20 -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough \
      -Wmissing-designated-field-initializers -I source/core/src -I generated \
      -isystem ${hosq}/include -isystem ${flatbuffers}/include \
      source/core/src/schema.cc source/core/src/host/document_edits.cc \
      source/core/src/util/crypto.cc source/core/src/host/templates.cc host_templates.cc \
      source/tests/core/agent/document_edit_driver.cc \
      ${
        lib.optionalString (
          !clangStdenv.hostPlatform.isDarwin
        ) "-isystem ${openssl.dev}/include -L${openssl.out}/lib -lcrypto"
      } \
      -o document-edit-driver
    export TSUGUMI_DOCUMENT_EDIT_DRIVER="$PWD/document-edit-driver"
    cd source/web
    npm run typecheck
    npm test
    touch $out
  ''
