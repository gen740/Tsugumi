# packages: tsugumid, the Web UI, the test packages CI runs, and on Linux the
# `tsugumi` command and the shared VM's boot disk.
{ inputs, ... }:
let
  inherit (inputs) nixpkgs hosq;
  # The core's own package set, used to build plans and target workers.
  cardNixpkgs = {
    rev = nixpkgs.rev or "dirty";
    narHash = nixpkgs.narHash;
  };
  lock = builtins.fromJSON (builtins.readFile ../../flake.lock);
  testNixPathLock.entries =
    map
      (
        name:
        let
          inherit (lock.nodes.${lock.nodes.root.inputs.${name}}) locked original;
        in
        {
          inherit name;
          inherit (locked) rev narHash;
          url = "${original.type}:${original.owner}/${original.repo}/${original.ref}";
        }
      )
      [
        "nixpkgs"
        "tsugumi-kit"
      ];
in
{
  perSystem =
    {
      pkgs,
      lib,
      ...
    }:
    let
      core = pkgs.callPackage ../core.nix {
        inherit cardNixpkgs hosq testNixPathLock;
        webgpu-headers = inputs.webgpu-headers or null;
      };
      ci-runtime = pkgs.callPackage ../ci-runtime.nix { inherit core; };
      ci-core = pkgs.callPackage ../core-tests.nix {
        inherit core hosq;
        runtime = ci-runtime;
        mock = ci-mock;
      };
      ci-mock = ci-core.override { target = "tsugumid-mock"; };
      client-wasm = pkgs.callPackage ../client-wasm.nix { };
      web = pkgs.callPackage ../web.nix { inherit client-wasm; };
      kernel = pkgs.callPackage ../../vm/guest/kernel.nix { };
      # Formatting runs on the host; guest binaries need no formatter paths.
      guestCore = core.override { withFormatters = false; };
      vm-system = pkgs.callPackage ../../vm/guest/vm-system.nix { tsugumid = guestCore; };
      vm-boot = pkgs.callPackage ../../vm/guest/vm-boot.nix { inherit kernel vm-system; };
    in
    {
      packages = {
        inherit
          ci-core
          ci-runtime
          ci-mock
          client-wasm
          web
          ;
        ci-vm-tests = ci-core.override { target = "tsugumi_vm_lifetime_tests"; };
        # Current native sources plus the guest definition; no app/site assets.
        guest-source = pkgs.runCommand "tsugumi-guest-source" { } ''
          mkdir -p $out
          cp -R ${core.src}/. $out/
          chmod -R u+w $out
          cp ${../../flake.nix} $out/flake.nix
          cp ${../../flake.lock} $out/flake.lock
          mkdir -p $out/nix
          cp -R ${../.}/. $out/nix/
          chmod -R u+w $out/nix
          cp -R ${../../vm/guest}/. $out/vm/guest/
        '';
        site-player = web.sitePlayer;
        ci-python = pkgs.callPackage ../ci-python.nix { };
        default = core;
        tsugumid = core;
      }
      // lib.optionalAttrs pkgs.stdenv.hostPlatform.isDarwin {
        ci-core-native = (ci-core.override { nativeDarwin = true; }).overrideAttrs (old: {
          postInstall = old.postInstall + ''
            for binary in ${core}/libexec/tsugumi-tests/tsugumi-view-*-tests; do
              ln -s "$binary" "$out/libexec/tsugumi-tests/"
            done
          '';
        });
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
        ci-arm-shell = ci-core.override { target = "tsugumi_arm_shell_tests"; };
        # The guest of tsugumi-pack and of the apps it exports (pack/).
        pack-guest = pkgs.callPackage ../../pack/guest {
          inherit kernel;
          tsugumid = guestCore;
        };
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
