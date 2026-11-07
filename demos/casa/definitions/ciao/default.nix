{ pkgs, inputs, params }:
# CIAO is fixed like the observation in load: the flake by its commit and hash
# (nixSources in tsugumi.json), which fixes CIAO's source, then built once by Nix.
let
  ciao = inputs.sources.ciao.packages.ciao;
in
{
  run = ''
    echo "ciao: ${ciao} (${ciao.name})"
    printf '{"tool": {"path": "%s", "version": "%s"}}' "${ciao}" "${(builtins.parseDrvName ciao.name).version}" > "$TSUGUMI_OUTPUT"
  '';
}
