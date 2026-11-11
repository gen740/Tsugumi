# The `tsugumi` command on Linux: the web server (web/) with tsugumid on this
# host and a terminal module built for this Node. cli/install.sh builds this.
{ lib, buildNpmPackage, importNpmLock, nodejs, flatbuffers, python3, makeWrapper, tsugumid }:
let
  files = lib.fileset.unions [
    ../web/package.json ../web/package-lock.json ../web/scripts ../web/server ../web/src ../web/tsconfig.json
    ../agent ../ui/modules ../protocol ../demos ../sdk ../flake.lock ../VERSION
    ../docs/site/player-data ../docs/assets/tsugumi-icon.svg
  ];
in
buildNpmPackage {
  pname = "tsugumi";
  version = lib.fileContents ../VERSION;
  src = lib.fileset.toSource { root = ../.; fileset = files; };
  inherit nodejs;
  npmRoot = "web";
  npmDeps = importNpmLock { npmRoot = ../web; };
  npmConfigHook = importNpmLock.npmConfigHook;
  nativeBuildInputs = [ flatbuffers python3 makeWrapper ];
  buildPhase = ''
    runHook preBuild
    (cd web && npm run gen && npm run build)
    runHook postBuild
  '';
  doCheck = false;
  dontNpmInstall = true;
  installPhase = ''
    runHook preInstall
    share=$out/share/tsugumi
    mkdir -p $share/web/node_modules $share/agent
    # The server finds these from web/dist/server.js, as in the repository.
    cp -R web/dist $share/web/dist
    # VERSION: `tsugumi update` says when this is already the latest.
    cp -R demos flake.lock VERSION $share/
    cp -R agent/Reference $share/agent/
    cp -R sdk $share/sdk
    # A native module, loaded when a terminal opens.
    cp -RL web/node_modules/node-pty $share/web/node_modules/
    makeWrapper ${nodejs}/bin/node $out/bin/tsugumi \
      --add-flags "$share/web/dist/server.js --open --reuse" \
      --set-default TSUGUMID ${tsugumid}/bin/tsugumid \
      --set-default TSUGUMI_WORKER ${tsugumid}/bin/tsugumi-worker \
      --set-default TSUGUMI_SDK_DIR ${tsugumid}/share/tsugumi/sdk
    runHook postInstall
  '';
  meta = {
    description = "Tsugumi: the Web UI and its host server";
    mainProgram = "tsugumi";
    platforms = lib.platforms.linux;
  };
}
