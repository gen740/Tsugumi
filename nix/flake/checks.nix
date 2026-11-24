# checks (`nix flake check`) and the formatter (`nix fmt`): nixfmt for Nix,
# clang-format (.clang-format) for C and C++.
# The core's tests are not here: `ci-core` packages their binaries and runner,
# and CI runs them outside the build sandbox (.github/workflows/ci-platform.yml).
{ inputs, ... }:
{
  imports = [ ./view-abi.nix ];
  perSystem =
    {
      pkgs,
      system,
      config,
      ...
    }:
    let
      inherit (config.packages) web;
      # Reuse the installed production host archives and compile only the
      # operation fixture. Web checks do not build the other C++ test targets.
      operationDriver = config.packages.ci-core.override {
        target = "tsugumi_document_edit_driver";
      };
      abi = pkgs.runCommand "tsugumi-kit-card-abi" { nativeBuildInputs = [ pkgs.python3 ]; } ''
        python3 ${../check-card-abi.py} ${../../protocol} ${inputs.tsugumi-kit}/abi
        touch "$out"
      '';
    in
    {
      checks = {
        card-abi = abi;
        # The pinned pair, kept apart from the core's own checks: the kit
        # revision in flake.lock passes its own checks (which the kit owns)
        # and shares this core's Card and Dawn view ABIs. No core check depends on a
        # particular kit product.
        pinned-kit = pkgs.linkFarm "tsugumi-pinned-kit-checks" (
          inputs.tsugumi-kit.checks.${system} // { card-abi = abi; }
        );
        web-typecheck = web.override { typecheckOnly = true; };
        web = pkgs.callPackage ../web-checks.nix {
          inherit web operationDriver;
        };
      };
      formatter = pkgs.treefmt.withConfig {
        runtimeInputs = [
          pkgs.nixfmt
          pkgs.llvmPackages.clang-unwrapped
        ];
        settings = {
          tree-root-file = "flake.nix";
          formatter.nixfmt = {
            command = "nixfmt";
            includes = [ "*.nix" ];
          };
          formatter.clang-format = {
            command = "clang-format";
            options = [ "-i" ];
            includes = [
              "*.c"
              "*.cc"
              "*.cpp"
              "*.h"
              "*.hh"
            ];
            # Demo Cards are formatted by the app's own Card formatter.
            excludes = [ "demos/*" ];
          };
        };
      };
    };
}
