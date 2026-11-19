# checks (`nix flake check`) and the formatter (`nix fmt`): nixfmt for Nix,
# clang-format (.clang-format) for C and C++.
# The core's tests are not here: `ci-core` packages their binaries and runner,
# and CI runs them outside the build sandbox (.github/workflows/ci-platform.yml).
{ ... }:
{
  perSystem =
    { pkgs, config, ... }:
    let
      inherit (config.packages) web;
      # Reuse the installed production host archives and compile only the
      # operation fixture. Web checks do not build the other C++ test targets.
      operationDriver = config.packages.ci-core.overrideAttrs {
        pname = "tsugumi-operation-test-driver";
        buildPhase = ''
          runHook preBuild
          cmake --build . --target tsugumi_document_edit_driver -j
          runHook postBuild
        '';
        installPhase = ''
          mkdir -p "$out/bin"
          cp tsugumi_document_edit_driver "$out/bin/"
        '';
        postInstall = "";
      };
    in
    {
      checks = {
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
