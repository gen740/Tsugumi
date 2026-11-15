# A C++ listener/supervisor/runner; no Python runtime or HTTP adapter.
{
  core,
  lib,
  makeWrapper,
  nix,
  runCommand,
  util-linux,
}:
runCommand "tsugumi-workerd" { nativeBuildInputs = [ makeWrapper ]; } ''
  mkdir -p "$out/bin"
  # Nix's build hook and the namespace launcher are the only external tools.
  makeWrapper ${core}/bin/tsugumi-workerd "$out/bin/tsugumi-workerd" \
    --prefix PATH : ${
      lib.makeBinPath [
        nix
        util-linux
      ]
    }
''
