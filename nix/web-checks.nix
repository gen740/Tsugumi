# Behavior checks consume the exact package build's dependencies.
# A test-only edit reruns this derivation without rebuilding the Web bundle.
{
  lib,
  stdenv,
  chromium,
  dejavu_fonts,
  fetchurl,
  flatbuffers,
  makeFontsConf,
  unzip,
  runCommand,
  nodejs,
  operationDriver,
  web,
}:
let
  # Nixpkgs Chromium is Linux-only. Use the matching standalone headless
  # browser on Mac so the same layout test runs in check-fast there.
  macBrowser = fetchurl {
    url = "https://storage.googleapis.com/chrome-for-testing-public/156.0.8078.73/mac-arm64/chrome-headless-shell-mac-arm64.zip";
    hash = "sha256-USx9NM1bZdYXo5pIkVtJDu1CCKY+nUew/XiT6DvoV2U=";
  };
in
runCommand "tsugumi-web-checks"
  {
    nativeBuildInputs = [
      flatbuffers
      nodejs
    ]
    ++ lib.optional stdenv.hostPlatform.isDarwin unzip;
    testSources = lib.fileset.toSource {
      root = ../.;
      fileset = lib.fileset.unions [
        ../ui/web/scripts/test.mjs
        ../ui/web/tests
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
    export TSUGUMI_WEB_PUBLIC_DIR=${web}/share/tsugumi/ui/web/dist/public
    export TSUGUMI_DOCUMENT_EDIT_DRIVER=${operationDriver}/libexec/tsugumi-tests/tsugumi_document_edit_driver
    ${
      if stdenv.hostPlatform.isDarwin then
        ''
          unzip -q ${macBrowser}
          export TSUGUMI_CHROMIUM="$PWD/chrome-headless-shell-mac-arm64/chrome-headless-shell"
        ''
      else
        ''
          export TSUGUMI_CHROMIUM=${lib.getExe chromium}
          # The sandbox has no fonts; without one, Chromium lays out text rows
          # with no height and the log view test measures nothing.
          export FONTCONFIG_FILE=${makeFontsConf { fontDirectories = [ dejavu_fonts ]; }}
        ''
    }
    cd source/ui/web
    npm test
    touch $out
  ''
