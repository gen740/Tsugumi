{ params, inputs }:
# Import the CIAO recipe from the project lock and build it with the project nixpkgs.
let
  pkgs = import <nixpkgs> { };
  ciao = import <ciao/src/package.nix> { inherit pkgs; };
in
{
  run = ''
    echo "ciao: ${ciao} (${ciao.name})"
    printf '{"tool": {"path": "%s", "version": "%s"}}' "${ciao}" "${(builtins.parseDrvName ciao.name).version}" > "$TSUGUMI_OUTPUT"
  '';
}
