{
  description = "Tsugumi: Card graph execution with pinned Nix environments";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  # Typed-schema JSON decoding/encoding for the C++ core (header only).
  inputs.hosq = {
    url = "github:CLI20-dev/hosq/38c3c8e21f570a9698006c361fc58bf0365896e0";
    flake = false;
  };

  outputs = { self, nixpkgs, hosq }:
    let
      systems = [ "aarch64-darwin" "aarch64-linux" "x86_64-linux" ];
      forAll = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
      # The nixpkgs revision Cards build against unless a project pins another.
      # Recorded so that the core can fetch it in pure evaluation mode.
      cardNixpkgs = {
        rev = nixpkgs.rev or "dirty";
        narHash = nixpkgs.narHash;
      };
    in {
      packages = forAll (pkgs:
        let
          core = pkgs.callPackage ./nix/core.nix { inherit self cardNixpkgs hosq; };
          kernel = pkgs.callPackage ./macOS/Container/kernel.nix { };
          vm-boot = pkgs.callPackage ./macOS/Container/vm-boot.nix { inherit kernel; };
        in {
          default = core;
          tsugumid = core;
        } // pkgs.lib.optionalAttrs pkgs.stdenv.hostPlatform.isLinux {
          inherit kernel vm-boot;
          # Tsugumi in the shared VM: built inside the VM at boot from this source.
          vm-system = pkgs.callPackage ./macOS/Container/vm-system.nix { tsugumid = core; };
          # What the app bundles: the boot disk (kernel and a small rootfs with Nix).
          vm = pkgs.runCommand "tsugumi-vm" { } ''
            mkdir -p $out
            ln -s ${vm-boot}/vmlinux ${vm-boot}/rootfs.erofs $out/
            echo ${baseNameOf vm-boot} > $out/vm-version
          '';
        });

      devShells = forAll (pkgs: {
        # clang as for the package (GCC 16 takes very long on the core's sources).
        default = (pkgs.mkShell.override { stdenv = pkgs.clangStdenv; }) {
          packages = with pkgs; [
            cmake
            ninja
            flatbuffers
            sqlite
            libgit2
            pkg-config
            # The embedded Nix evaluator (headers and libraries of the Nix in use).
            nix.libs.nix-expr
            nix.libs.nix-store
            nix.libs.nix-fetchers
            nix.libs.nix-util
            git
            (python3.withPackages (ps: [ ps.flatbuffers ps.pytest ps.websockets ]))
            # Card shell steps use jq (the package gives them the same tools).
            jq
            # Source formatters used by ProjectFileFormat (also in the image).
            ruff
            nixfmt
            shfmt
            # The web UI (web/) and its contract test with the Swift Agent.
            nodejs
          ] ++ lib.optionals stdenv.hostPlatform.isLinux [ bubblewrap openssl ];
          MACOSX_DEPLOYMENT_TARGET = "26.0";
          HOSQ_INCLUDE_DIR = "${hosq}/include";
          TSUGUMI_CARD_NIXPKGS_REV = cardNixpkgs.rev;
          TSUGUMI_CARD_NIXPKGS_NARHASH = cardNixpkgs.narHash;
        };
      });
    };
}
