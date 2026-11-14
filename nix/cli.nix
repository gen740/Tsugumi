# Assemble independently cached native and Web artifacts without rebuilding
# either. The Web server resolves runtime assets relative to its own output.
{
  lib,
  runCommand,
  makeWrapper,
  coreutils,
  nodejs,
  tsugumid,
  web,
}:
runCommand "tsugumi-${lib.fileContents ../VERSION}"
  {
    nativeBuildInputs = [ makeWrapper ];
    meta = {
      description = "Tsugumi: the Web UI and its host server";
      mainProgram = "tsugumi";
      platforms = lib.platforms.linux;
    };
  }
  ''
    mkdir -p $out/bin $out/share
    ln -s ${web}/share/tsugumi $out/share/tsugumi
    makeWrapper ${nodejs}/bin/node $out/bin/tsugumi \
      --add-flags "${web}/share/tsugumi/web/dist/server.js --open --reuse" \
      --prefix PATH : ${
        lib.makeBinPath [
          coreutils
        ]
      } \
      --set-default TSUGUMID ${tsugumid}/bin/tsugumid \
      --set-default TSUGUMI_AGENTD ${tsugumid}/bin/tsugumi-agentd \
      --set-default TSUGUMI_WORKER ${tsugumid}/bin/tsugumi-worker \
      --set-default TSUGUMI_SDK_DIR ${tsugumid}/share/tsugumi/sdk
  ''
