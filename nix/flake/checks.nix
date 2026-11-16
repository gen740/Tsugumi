# checks (`nix flake check`) and the formatter of the Nix files (`nix fmt`).
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
      formatter = pkgs.nixfmt-tree;
    };
}
