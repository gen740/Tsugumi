{
  description = "Tsugumi: Card graph execution with pinned Nix environments";

  nixConfig = {
    extra-substituters = [ "https://tsugumi.cachix.org" ];
    extra-trusted-public-keys = [ "tsugumi.cachix.org-1:am26Q4VFWYLyJ+t+O5j03MmMVmZ2Ifw6cbNUoLhlrhI=" ];
  };

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  # Typed-schema JSON decoding/encoding for the C++ core (header only).
  inputs.hosq = {
    url = "github:CLI20-dev/hosq/1eb894224b0feffbb5b81d075d3e2c76160f99a1";
    flake = false;
  };

  # Match the wasm32 WebGPU ABI in protocol/include/tsugumi/tsugumi_view.h.
  inputs.webgpu-headers = {
    url = "github:webgpu-native/webgpu-headers/a1dc77409bb2b8bd80540cbf4b8db219e123e909";
    flake = false;
  };

  inputs.flake-parts = {
    url = "github:hercules-ci/flake-parts";
    inputs.nixpkgs-lib.follows = "nixpkgs";
  };

  inputs.tsugumi-kit = {
    url = "github:gen740/tsugumi-kit/feat/view-send-by-path";
    inputs.nixpkgs.follows = "nixpkgs";
    inputs.flake-parts.follows = "flake-parts";
  };

  outputs =
    inputs:
    inputs.flake-parts.lib.mkFlake { inherit inputs; } {
      systems = [
        "aarch64-darwin"
        "aarch64-linux"
        "x86_64-linux"
      ];
      imports = [
        ./nix/flake/packages.nix
        ./nix/flake/checks.nix
        ./nix/flake/docs.nix
        ./nix/flake/tools.nix
        ./nix/flake/dev-shell.nix
      ];
    };
}
