{ params, inputs }:
let
  pkgs = import <nixpkgs> { };
  sdk = import <tsugumi-kit/sdk/python> { inherit pkgs; };
  common = {
    inherit inputs params;
    outputs = ./_outputs.fbs;
  };
in
{
  run = sdk.pythonCard (common // {
    src = ./src;
  });
  api = sdk.pythonApi common;
  views = sdk.views;
}
