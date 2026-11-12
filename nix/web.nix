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
  ];
  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [
      ../web/package.json
      ../web/package-lock.json
      ../web/scripts/build.mjs
      ../web/scripts/build-client.sh
      ../web/scripts/gen.sh
      ../web/server
      ../web/src
      ../web/tsconfig.json
      ../agent/Shared/agent.json
      ../agent/Shared/guide-script.json
      ../agent/Shared/guide-script.en.json
      ../agent/Shared/demo-skill.md
      ../agent/Reference
      ../ui/modules
      ../ui/templates
      ../protocol/agent.fbs
      ../protocol/tsugumi.fbs
      ../protocol/tsugumi_enums.fbs
      ../protocol/data.fbs
      ../demos
      ../sdk
      ../flake.lock
      ../VERSION
      ../docs/site/player-data
      ../docs/assets/tsugumi-icon.svg
    ];
  };
  inherit nodejs;
  npmRoot = "web";
  npmDeps = importNpmLock { npmRoot = ../web; };
  npmConfigHook = importNpmLock.npmConfigHook;
  nativeBuildInputs = [
    flatbuffers
    python3
  ];
  TSUGUMI_WASM_CLIENT_DIR = "${client-wasm}/share/tsugumi/client";
  buildPhase = ''
    runHook preBuild
    (cd web && npm run gen && npm run build)
    runHook postBuild
  '';
  dontNpmInstall = true;
  installPhase = ''
    runHook preInstall
    share=$out/share/tsugumi
    mkdir -p $share/web/node_modules $share/agent $testSupport
    cp -R web/dist $share/web/dist
    cp -R demos flake.lock VERSION $share/
    cp -R agent/Reference $share/agent/
    cp -R sdk $share/sdk
    # Native addon installation remains part of the Node package build. Keep
    # its platform binaries beside the server for runtime module resolution.
    cp -RL web/node_modules/node-pty $share/web/node_modules/
    find $share/web/node_modules/node-pty -type f -name spawn-helper -exec chmod a+x {} +
    # Checks reuse installation and generated bindings, without putting the
    # development tools in the distributed Web output's runtime closure.
    cp -R web/node_modules $testSupport/node_modules
    cp -R web/src/gen $testSupport/gen
    runHook postInstall
  '';
  meta.description = "Tsugumi Web UI and platform host adapter";
}
