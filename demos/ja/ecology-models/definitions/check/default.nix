{ params, inputs }:
let
  pkgs = import <nixpkgs> { };
  sdk = import <tsugumi-kit/sdk/julia> { inherit pkgs; };
  common = {
    inherit inputs params;
    outputs = ./_outputs.fbs;
  };
in
{
  run = sdk.juliaCard (common // {
    src = ./src;
  });
  api = sdk.juliaApi common;
  views = sdk.views;
}
