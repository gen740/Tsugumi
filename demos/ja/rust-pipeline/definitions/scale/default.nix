{ params, inputs }:
let
  pkgs = import <nixpkgs> { };
  sdk = import <tsugumi-kit/sdk/rust> { inherit pkgs; };
  common = {
    inherit inputs;
    outputs = ./_outputs.fbs;
    params = ./params.nix;
  };
in
{
  run = sdk.rustCard (common // {
    src = ./src;
  });
  api = sdk.rustApi common;
  views = sdk.views;
}
