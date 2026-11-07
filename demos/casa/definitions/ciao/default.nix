{ pkgs, inputs, params }:
# CIAO is fixed like the observation in load: the packaging by its commit and
# hash (nixSources in tsugumi.json), the source by hash, then built once by Nix.
let
  ciao = import "${inputs.sources.ciao}/src/package.nix" { inherit pkgs; };
in
{
  run = ''
    echo "ciao: ${ciao} (${ciao.version})"
    printf '{"tool": {"path": "%s", "version": "%s"}}' "${ciao}" "${ciao.version}" > "$TSUGUMI_OUTPUT"
  '';
}
