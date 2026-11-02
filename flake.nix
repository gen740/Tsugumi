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
      systems = [ "aarch64-darwin" "aarch64-linux" ];
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
        in {
          default = core;
          tsugumid = core;
        } // pkgs.lib.optionalAttrs pkgs.stdenv.isLinux {
          image = pkgs.callPackage ./macOS/Container/image.nix { tsugumid = core; };
        });

      devShells = forAll (pkgs: {
        default = pkgs.mkShell {
          packages = with pkgs; [
            cmake
            ninja
            flatbuffers
            sqlite
            pkg-config
            git
            (python3.withPackages (ps: [ ps.flatbuffers ps.pytest ps.websockets ]))
            # Source formatters used by ProjectFileFormat (also in the image).
            ruff
            nixfmt-rfc-style
            shfmt
          ] ++ lib.optionals stdenv.hostPlatform.isLinux [ bubblewrap ];
          MACOSX_DEPLOYMENT_TARGET = "26.0";
          HOSQ_INCLUDE_DIR = "${hosq}/include";
          TSUGUMI_CARD_NIXPKGS_REV = cardNixpkgs.rev;
          TSUGUMI_CARD_NIXPKGS_NARHASH = cardNixpkgs.narHash;
        };
      });
    };
}
