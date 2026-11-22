# Evaluate the kit flake with the Card's pkgs, including its system and pin.
{
  pkgs,
  kit,
  flakeParts,
}:
let
  system = pkgs.stdenv.hostPlatform.system;
  parts = (import (flakeParts + "/flake.nix")).outputs {
    self = {
      outPath = toString flakeParts;
    };
    nixpkgs-lib.lib = pkgs.lib;
  };
  outputs = (import (kit + "/flake.nix")).outputs {
    self = outputs // {
      outPath = toString kit;
      inputs = { inherit nixpkgs; };
    };
    flake-parts = parts;
    inherit nixpkgs;
  };
  nixpkgs = {
    _type = "flake";
    legacyPackages.${system} = pkgs;
  };
in
outputs.lib.${system}
