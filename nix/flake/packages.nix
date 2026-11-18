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
      libgit2-local = pkgs.callPackage ../libgit2-local.nix { };
      client-wasm = pkgs.callPackage ../client-wasm.nix { };
      web = pkgs.callPackage ../web.nix { inherit client-wasm; };
      kernel = pkgs.callPackage ../../vm/guest/kernel.nix { };
      vm-system = pkgs.callPackage ../../vm/guest/vm-system.nix {
        # Formatting runs on the host; guest binaries need no formatter paths.
        tsugumid = core.override { withFormatters = false; };
      };
      vm-boot = pkgs.callPackage ../../vm/guest/vm-boot.nix { inherit kernel vm-system; };
    in
    {
      packages = {
        inherit
          ci-core
          client-wasm
          libgit2-local
          web
          ;
        site-player = web.sitePlayer;
        ci-python = pkgs.callPackage ../ci-python.nix { };
        default = core;
        tsugumid = core;
      }
      // lib.optionalAttrs pkgs.stdenv.hostPlatform.isLinux {
        workerd = pkgs.callPackage ../workerd.nix { inherit core; };
        # The `tsugumi` command (cli/install.sh): the Web UI with tsugumid on this host.
        cli = pkgs.callPackage ../cli.nix {
          tsugumid = core;
          inherit web;
        };
      }
      // lib.optionalAttrs (pkgs.stdenv.hostPlatform.system == "aarch64-linux") {
        inherit kernel vm-boot vm-system;
        # One distributable image; the helper extracts its VZ attachments.
        vm =
          pkgs.runCommand "tsugumi-vm"
            {
              nativeBuildInputs = [
                pkgs.gnutar
                pkgs.gzip
              ];
            }
            ''
              mkdir -p $out image
              cp ${vm-boot}/vmlinux ${vm-boot}/rootfs.erofs image/
              echo ${baseNameOf vm-boot} > image/vm-version
              tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner \
                -C image -cf - vmlinux rootfs.erofs vm-version | gzip -n > $out/tsugumi-vm.tar.gz
              cp image/vm-version $out/
            '';
      };
    };
}
