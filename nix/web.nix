# The Web UI (hostd serves dist/public) and its runtime assets. No native core
# build is included here; the Linux command injects those executables.
{
  lib,
  buildNpmPackage,
  importNpmLock,
  nodejs,
  flatbuffers,
  python3,
  client-wasm,
  typecheckOnly ? false,
}:
buildNpmPackage {
  pname = if typecheckOnly then "tsugumi-web-typecheck" else "tsugumi-web";
  version = lib.fileContents ../VERSION;
  outputs = [
    "out"
  ]
  ++ lib.optionals (!typecheckOnly) [
    "testSupport"
    "sitePlayer"
  ];
  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions (
      [
        ../ui/web/package.json
        ../ui/web/package-lock.json
        ../ui/web/scripts/check-boundaries.mjs
        ../ui/web/scripts/gen.sh
        ../ui/web/src
        ../ui/web/shared
        ../ui/web/tsconfig.json
        ../ui/web/tsconfig.base.json
        ../ui/web/tsconfig.shared.json
        ../ui/web/tsconfig.browser.json
        ../hostd/agent/Shared/agent.json
        ../hostd/agent/Shared/demo-skill.md
        ../ui/shared/modules
        ../ui/shared/templates
        (lib.fileset.fileFilter (file: file.hasExt "fbs") ../protocol)
      ]
      ++ lib.optionals (!typecheckOnly) [
        ../ui/web/scripts/build.mjs
        ../ui/web/scripts/build-client.sh
        ../ui/web/third-party
        ../third-party/emscripten
        ../third-party/flatbuffers
        ../demos
        ../flake.lock
        ../VERSION
        ../docs/site/player/build.mjs
        ../docs/site/player/narration.json
        ../docs/site/player-data/assets
        ../docs/CardGuide.md
        ../docs/assets/tsugumi-icon.svg
      ]
    );
  };
  inherit nodejs;
  npmRoot = "ui/web";
  npmDeps = importNpmLock { npmRoot = ../ui/web; };
  npmConfigHook = importNpmLock.npmConfigHook;
  nativeBuildInputs = [
    flatbuffers
    python3
  ];
  TSUGUMI_WASM_CLIENT_DIR = if typecheckOnly then null else "${client-wasm}/share/tsugumi/client";
  buildPhase =
    if typecheckOnly then
      ''
        runHook preBuild
        cd ui/web
        npm run gen
        npm run typecheck
        runHook postBuild
      ''
    else
      ''
        runHook preBuild
        (cd ui/web && npm run gen)
        # Build once with this package's pinned dependencies and generated bindings.
        # The site, thumbnails and tests all consume this same output.
        node docs/site/player/build.mjs --out-dir "$sitePlayer"
        export TSUGUMI_SITE_PLAYER_DIR="$sitePlayer"
        (cd ui/web && npm run build)
        runHook postBuild
      '';
  dontNpmInstall = true;
  installPhase =
    if typecheckOnly then
      "touch $out"
    else
      ''
        runHook preInstall
        share=$out/share/tsugumi
        mkdir -p $share/ui/web $testSupport
        cp -R ui/web/dist $share/ui/web/dist
        cp -R demos flake.lock VERSION $share/
        # Checks reuse installation and generated bindings, without putting the
        # development tools in the distributed Web output's runtime closure.
        cp -R ui/web/node_modules $testSupport/node_modules
        cp -R ui/web/shared/gen $testSupport/gen
        runHook postInstall
      '';
  meta.description = "Tsugumi Web UI";
}
