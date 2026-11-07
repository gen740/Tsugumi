{ pkgs, inputs, params }:
# CIAO is fixed like the observation in load: its source by hash, then built
# once by Nix. package.nix and ciao-setup-hook.sh are copies from
# https://github.com/gen740/ciao at 0bf0f8b (a Card's Nix evaluation can only fetch nixpkgs).
let
  ciao = import ./package.nix { inherit pkgs; };
in
{
  run = ''
    echo "ciao: ${ciao} (${ciao.version})"
    printf '{"tool": {"path": "%s", "version": "%s"}}' "${ciao}" "${ciao.version}" > "$TSUGUMI_OUTPUT"
  '';
}
