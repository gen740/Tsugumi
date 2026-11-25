# Assemble independently cached native and Web artifacts without rebuilding
# either: the C++ tsugumi command and hostd from the core, the built page
# (hostd serves it) and demos from the Web output.
{
  lib,
  runCommand,
  makeWrapper,
  tsugumid,
  web,
}:
runCommand "tsugumi-${lib.fileContents ../VERSION}"
  {
    nativeBuildInputs = [ makeWrapper ];
    meta = {
      description = "Tsugumi: the tsugumi command and the host that serves the Web UI";
      mainProgram = "tsugumi";
      platforms = lib.platforms.linux;
    };
  }
  ''
    mkdir -p $out/bin $out/share/licenses
    cp -R ${../third-party}/. $out/share/licenses/
    ln -s ${web}/share/tsugumi $out/share/tsugumi
    makeWrapper ${tsugumid}/bin/tsugumi $out/bin/tsugumi \
      --set TSUGUMI_DEMOS_DIR ${web}/share/tsugumi/demos \
      --set-default TSUGUMI_REPO ${web}/share/tsugumi \
      --set-default TSUGUMID ${tsugumid}/bin/tsugumid \
      --set-default TSUGUMI_HOSTD ${tsugumid}/bin/tsugumi-hostd \
      --set-default TSUGUMI_WORKER ${tsugumid}/bin/tsugumi-worker \
      --set-default TSUGUMI_NIX_DIR ${tsugumid}/share/tsugumi/nix
  ''
