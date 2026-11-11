# The site player uses the Web package's exact Node dependencies and bindings.
# CardGuide.md is an opaque build input parsed only by the existing generator.
{
  lib,
  runCommand,
  nodejs,
  web,
}:
runCommand "tsugumi-site-player-check"
  {
    nativeBuildInputs = [ nodejs ];
    playerSources = lib.fileset.toSource {
      root = ../.;
      fileset = lib.fileset.unions [
        ../docs/site/player/build.mjs
        ../docs/site/player/narration.json
        ../docs/site/js/player.js
        ../docs/site/player.css
        ../docs/CardGuide.md
      ];
    };
  }
  ''
    export HOME="$TMPDIR"
    cp -R ${web.src} source
    chmod -R u+w source
    cp -R $playerSources/. source/
    ln -s ${web.testSupport}/node_modules source/web/node_modules
    cp -R ${web.testSupport}/gen source/web/src/gen
    cd source
    node docs/site/player/build.mjs --check
    touch $out
  ''
