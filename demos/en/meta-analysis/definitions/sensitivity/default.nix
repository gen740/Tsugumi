{ params, inputs }:
let
  pkgs = import <nixpkgs> { };
  sdk = import <tsugumi-kit/sdk/r> { inherit pkgs; };
  common = {
    inherit inputs params;
    outputs = ./_outputs.fbs;
  };
in
{
  run = sdk.rCard (common // {
    src = ./src;
    packages = ps: [
      ps.metafor
      ps.ggplot2
    ];
  });
  api = sdk.rApi common;
  views = sdk.views;
}
