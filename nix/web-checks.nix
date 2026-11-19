# Behavior checks consume the exact package build's dependencies.
# A test-only edit reruns this derivation without rebuilding the Web bundle.
{
  lib,
  runCommand,
  nodejs,
  operationDriver,
  web,
}:
runCommand "tsugumi-web-checks"
  {
    nativeBuildInputs = [
      nodejs
    ];
    testSources = lib.fileset.toSource {
      root = ../.;
      fileset = lib.fileset.unions [
        ../ui/web/scripts/test.mjs
        ../ui/web/tests
        ../sdk/views
        ../tests/bench/wire-frames.ts
        ../tests/fixtures
        ../hostd/agent/Shared/system-prompt.md
        ../hostd/agent/Shared/mcp-transport.md
      ];
    };
  }
  ''
    export HOME="$TMPDIR"
    cp -R ${web.src} source
    chmod -R u+w source
    cp -R $testSources/. source/
    ln -s ${web.testSupport}/node_modules source/ui/web/node_modules
    cp -R ${web.testSupport}/gen source/ui/web/shared/gen
    export TSUGUMI_SITE_PLAYER_DIR=${web.sitePlayer}
    export TSUGUMI_DOCUMENT_EDIT_DRIVER=${operationDriver}/libexec/tsugumi-tests/tsugumi_document_edit_driver
    cd source/ui/web
    npm test
    touch $out
  ''
