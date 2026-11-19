# Commands for working on the repository (`nix run .#<name>`), and the tools
# the host build scripts take from Nix.
{
  perSystem =
    { pkgs, lib, ... }:
    {
      apps = {
        guest-image = {
          program = lib.getExe (pkgs.callPackage ../guest-image.nix { });
          meta.description = "Put the guest image of the working tree in dist/tsugumi-vm (from an artifact; --build to build it here)";
        };
        release = {
          program = lib.getExe (pkgs.callPackage ../release.nix { });
          meta.description = "Cut a release: open the pull request that sets VERSION, then push the tag";
        };
      };
      # flatc for the Swift builds, which use Apple's toolchain in a clean
      # environment and take only flatc from Nix (the flake's nixpkgs).
      packages.flatc = pkgs.flatbuffers;
    };
}
