# apps: the documentation site's dev server, build and tests (nix/docs-site.nix).
{
  perSystem =
    { pkgs, lib, ... }:
    let
      site = pkgs.callPackage ../docs-site.nix { };
      describe = {
        dev-doc = "Serve the documentation site from the working tree with live reload";
        build-doc = "Build the documentation site into build/docs";
        test-doc = "Check the built documentation site";
      };
    in
    {
      apps = lib.mapAttrs (name: description: {
        program = site.${name};
        meta = { inherit description; };
      }) describe;
    };
}
