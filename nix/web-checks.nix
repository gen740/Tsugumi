# Type and behavior checks consume the exact package build's dependencies.
# A test-only edit reruns this derivation without rebuilding the Web bundle.
{
  lib,
  runCommand,
  nodejs,
  web,
}:
runCommand "tsugumi-web-checks"
  {
    nativeBuildInputs = [ nodejs ];
    testSources = lib.fileset.toSource {
      root = ../.;
      fileset = lib.fileset.unions [
        ../web/scripts/test.mjs
        ../web/tests
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
    cd source/web
    npm run typecheck
    npm test
    touch $out
  ''
