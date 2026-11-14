# Type and behavior checks consume the exact package build's dependencies.
# A test-only edit reruns this derivation without rebuilding the Web bundle.
{
  lib,
  runCommand,
  nodejs,
  clangStdenv,
  flatbuffers,
  hosq,
  web,
}:
runCommand "tsugumi-web-checks"
  {
    nativeBuildInputs = [
      nodejs
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
        ../core/src/agent/json.hh
        ../core/src/schema.hh
        ../core/src/schema.cc
        ../tests/core/agent/document_edit_driver.cc
        ../tests/fixtures
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
    mkdir -p generated/tsugumi/protocol
    flatc --cpp --gen-object-api --gen-all -I source/protocol \
      -o generated/tsugumi/protocol source/protocol/host.fbs
    ${clangStdenv.cc}/bin/clang++ -std=c++20 -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough \
      -Wmissing-designated-field-initializers -I source/core/src -I generated \
      -isystem ${hosq}/include -isystem ${flatbuffers}/include \
      source/core/src/schema.cc source/core/src/host/document_edits.cc \
      source/tests/core/agent/document_edit_driver.cc -o document-edit-driver
    export TSUGUMI_DOCUMENT_EDIT_DRIVER="$PWD/document-edit-driver"
    cd source/web
    npm run typecheck
    npm test
    touch $out
  ''
