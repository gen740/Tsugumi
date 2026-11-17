# The Web UI, its host adapter and runtime assets. No native core build is
# included here; the Linux command injects those executables in its wrapper.
{
  lib,
  buildNpmPackage,
  importNpmLock,
  nodejs,
  flatbuffers,
  python3,
  client-wasm,
}:
buildNpmPackage {
  pname = "tsugumi-web";
  version = lib.fileContents ../VERSION;
  outputs = [
    "out"
    "testSupport"
    "sitePlayer"
  ];
  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [
      ../ui/web/package.json
      ../ui/web/package-lock.json
      ../ui/web/scripts/check-boundaries.mjs
      ../ui/web/scripts/build.mjs
      ../ui/web/scripts/build-client.sh
      ../ui/web/scripts/gen.sh
      ../ui/web/server
      ../ui/web/src
      ../ui/web/shared
      ../ui/web/tsconfig.json
      ../ui/web/tsconfig.base.json
      ../ui/web/tsconfig.shared.json
      ../ui/web/tsconfig.browser.json
      ../ui/web/tsconfig.server.json
      ../hostd/agent/Shared/agent.json
      ../hostd/agent/Shared/demo-skill.md
      ../ui/shared/modules
      ../ui/shared/templates
      ../protocol/remote_worker.fbs
      ../protocol/local_worker.fbs
      ../protocol/service_common.fbs
      ../protocol/host.fbs
      ../protocol/vm_host.fbs
      ../protocol/agent.fbs
      ../protocol/tsugumi.fbs
      ../protocol/tsugumi_enums.fbs
      ../protocol/data.fbs
      ../demos
      ../sdk
      ../flake.lock
      ../VERSION
      ../docs/site/player/build.mjs
      ../docs/site/player/narration.json
      ../docs/site/player-data/assets
      ../docs/CardGuide.md
      ../docs/assets/tsugumi-icon.svg
    ];
  };
  inherit nodejs;
  npmRoot = "ui/web";
  npmDeps = importNpmLock { npmRoot = ../ui/web; };
  npmConfigHook = importNpmLock.npmConfigHook;
  nativeBuildInputs = [
    flatbuffers
    python3
  ];
  TSUGUMI_WASM_CLIENT_DIR = "${client-wasm}/share/tsugumi/client";
  buildPhase = ''
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
  installPhase = ''
    runHook preInstall
    share=$out/share/tsugumi
    mkdir -p $share/ui/web $share/agent $testSupport
    cp -R ui/web/dist $share/ui/web/dist
    cp -R demos flake.lock VERSION $share/
    cp -R sdk $share/sdk
    # Checks reuse installation and generated bindings, without putting the
    # development tools in the distributed Web output's runtime closure.
    cp -R ui/web/node_modules $testSupport/node_modules
    cp -R ui/web/shared/gen $testSupport/gen
    runHook postInstall
  '';
  meta.description = "Tsugumi Web UI and platform host adapter";
}
