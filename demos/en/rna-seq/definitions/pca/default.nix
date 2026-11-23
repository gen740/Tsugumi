{ params, inputs }:
let
  pkgs = import <nixpkgs> { };
  sdk = import <tsugumi-kit/sdk/r> { inherit pkgs; };
  common = {
    inherit inputs;
    outputs = ./_outputs.fbs;
    params = ./params.nix;
  };
in
{
  run = sdk.rCard (common // {
    src = ./src;
    packages = ps: [ ps.ggplot2 ];
  });
  api = sdk.rApi common;
  views = sdk.views;
}
