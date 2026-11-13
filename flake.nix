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

  outputs =
    {
      self,
      nixpkgs,
      hosq,
    }:
    let
      systems = [
        "aarch64-darwin"
        "aarch64-linux"
        "x86_64-linux"
      ];
      forAll = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
      # The nixpkgs revision Cards build against unless a project pins another.
      # Recorded so that the core can fetch it in pure evaluation mode.
      cardNixpkgs = {
        rev = nixpkgs.rev or "dirty";
        narHash = nixpkgs.narHash;
      };
      packagesFor =
        pkgs:
        let
          core = pkgs.callPackage ./nix/core.nix { inherit self cardNixpkgs hosq; };
          ci-core = pkgs.callPackage ./nix/core-tests.nix { inherit core hosq; };
          client-wasm = pkgs.callPackage ./nix/client-wasm.nix { };
          web = pkgs.callPackage ./nix/web.nix { inherit client-wasm; };
          kernel = pkgs.callPackage ./macOS/Container/kernel.nix { };
          vm-boot = pkgs.callPackage ./macOS/Container/vm-boot.nix { inherit kernel; };
        in
        {
          inherit ci-core client-wasm web;
          ci-python = pkgs.callPackage ./nix/ci-python.nix { };
          default = core;
          tsugumid = core;
        }
        // pkgs.lib.optionalAttrs pkgs.stdenv.hostPlatform.isLinux {
          inherit kernel vm-boot;
          remote-daemon = pkgs.callPackage ./nix/remote-daemon.nix { inherit core; };
          # Tsugumi in the shared VM: built inside the VM at boot from this source.
          vm-system = pkgs.callPackage ./macOS/Container/vm-system.nix { tsugumid = core; };
          # The `tsugumi` command (cli/install.sh): the Web UI with tsugumid on this host.
          cli = pkgs.callPackage ./nix/cli.nix {
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
    in
    {
      packages = forAll packagesFor;
      apps = forAll (pkgs: {
        # The documentation site's dev server on the working tree (docs/site).
        dev-doc = {
          type = "app";
          program = pkgs.lib.getExe (pkgs.callPackage ./nix/dev-doc.nix { });
          meta.description = "Serve the documentation site from the working tree with live reload";
        };
      });
      checks = forAll (pkgs: {
        web = pkgs.callPackage ./nix/web-checks.nix { web = (packagesFor pkgs).web; };
        site-player = pkgs.callPackage ./nix/site-player-check.nix { web = (packagesFor pkgs).web; };
      });

      devShells = forAll (pkgs: {
        # clang as for the package (GCC 16 takes very long on the core's sources).
        default = (pkgs.mkShell.override { stdenv = pkgs.clangStdenv; }) {
          packages =
            with pkgs;
            [
              cmake
              ninja
              flatbuffers
              sqlite
              libgit2
              curl
              gtest
              emscripten
              pkg-config
              # The embedded Nix evaluator (headers and libraries of the Nix in use).
              nix.libs.nix-expr
              nix.libs.nix-store
              nix.libs.nix-fetchers
              nix.libs.nix-flake
              nix.libs.nix-util
              git
              (python3.withPackages (ps: [
                ps.flatbuffers
                ps.pytest
                ps.websockets
                ps.matplotlib
                ps.plotly
                ps.altair
              ]))
              # Card shell steps use jq (the package gives them the same tools).
              jq
              # Source formatters used by ProjectFileFormat (also in the image).
              ruff
              nixfmt
              shfmt
              # The web UI (web/) and its contract test with the Swift Agent.
              nodejs
            ]
            ++ lib.optionals stdenv.hostPlatform.isLinux [
              bubblewrap
              openssl
            ];
          MACOSX_DEPLOYMENT_TARGET = "26.0";
          HOSQ_INCLUDE_DIR = "${hosq}/include";
          TSUGUMI_CARD_NIXPKGS_REV = cardNixpkgs.rev;
          TSUGUMI_CARD_NIXPKGS_NARHASH = cardNixpkgs.narHash;
        };
      });
    };
}
