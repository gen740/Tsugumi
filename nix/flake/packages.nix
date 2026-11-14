# packages: tsugumid, the Web UI, the test packages CI runs, and on Linux the
# `tsugumi` command and the shared VM's boot disk.
{ inputs, ... }:
let
  inherit (inputs) nixpkgs hosq;
  # The nixpkgs revision Cards build against unless a project pins another.
  # Recorded so that the core can fetch it in pure evaluation mode.
  cardNixpkgs = {
    rev = nixpkgs.rev or "dirty";
    narHash = nixpkgs.narHash;
  };
in
{
  perSystem =
    { pkgs, lib, ... }:
    let
      core = pkgs.callPackage ../core.nix { inherit cardNixpkgs hosq; };
      ci-core = pkgs.callPackage ../core-tests.nix { inherit core hosq; };
      client-wasm = pkgs.callPackage ../client-wasm.nix { };
      web = pkgs.callPackage ../web.nix { inherit client-wasm; };
      kernel = pkgs.callPackage ../../macOS/Container/kernel.nix { };
      vm-boot = pkgs.callPackage ../../macOS/Container/vm-boot.nix { inherit kernel; };
    in
    {
      packages = {
        inherit ci-core client-wasm web;
        site-player = web.sitePlayer;
        ci-python = pkgs.callPackage ../ci-python.nix { };
        default = core;
        tsugumid = core;
      }
      // lib.optionalAttrs pkgs.stdenv.hostPlatform.isLinux {
        inherit kernel vm-boot;
        remote-worker = pkgs.callPackage ../remote-worker.nix { inherit core; };
        # Tsugumi in the shared VM: built inside the VM at boot from this source.
        vm-system = pkgs.callPackage ../../macOS/Container/vm-system.nix { tsugumid = core; };
        # The `tsugumi` command (cli/install.sh): the Web UI with tsugumid on this host.
        cli = pkgs.callPackage ../cli.nix {
          tsugumid = core;
          inherit web;
        };
        # What the app bundles: the boot disk (kernel and a small rootfs with Nix).
        vm = pkgs.runCommand "tsugumi-vm" { } ''
          mkdir -p $out
          ln -s ${vm-boot}/vmlinux ${vm-boot}/rootfs.erofs $out/
          echo ${baseNameOf vm-boot} > $out/vm-version
        '';
      };
    };
}
