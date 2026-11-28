# A C++ listener/supervisor/runner; no Python runtime or HTTP adapter.
{
  core,
  lib,
  makeWrapper,
  nix,
  attic-client,
  cachix,
  runCommand,
  util-linux,
  # The source revision, reported to hosts and on the worker's dashboard.
  revision ? "",
}:
runCommand "tsugumi-workerd" { nativeBuildInputs = [ makeWrapper ]; } ''
  mkdir -p "$out/bin"
  # The namespace launcher, Nix and the supported binary cache clients.
  makeWrapper ${core}/bin/tsugumi-workerd "$out/bin/tsugumi-workerd" \
    --set TSUGUMI_WORKERD_REVISION ${lib.escapeShellArg revision} \
    --prefix PATH : ${
      lib.makeBinPath [
        nix
        attic-client
        cachix
        util-linux
      ]
    }
''
