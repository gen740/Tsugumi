# checks (`nix flake check`) and the formatter of the Nix files (`nix fmt`).
# The core's tests are not here: `ci-core` packages their binaries and runner,
# and CI runs them outside the build sandbox (.github/workflows/ci-platform.yml).
{
  perSystem =
    { pkgs, config, ... }:
    let
      inherit (config.packages) web;
    in
    {
      checks = {
        web = pkgs.callPackage ../web-checks.nix { inherit web; };
      };
      formatter = pkgs.nixfmt-tree;
    };
}
