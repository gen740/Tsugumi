# A C++ listener/supervisor/runner; no Python runtime or HTTP adapter.
{
  core,
  lib,
  makeWrapper,
  nix,
  runCommand,
  util-linux,
}:
runCommand "tsugumi-remote-worker" { nativeBuildInputs = [ makeWrapper ]; } ''
  mkdir -p "$out/bin"
  # Nix's build hook and the namespace launcher are the only external tools.
  makeWrapper ${core}/bin/tsugumi-remote-worker "$out/bin/tsugumi-remote-worker" \
    --prefix PATH : ${
      lib.makeBinPath [
        nix
        util-linux
      ]
    }
''
