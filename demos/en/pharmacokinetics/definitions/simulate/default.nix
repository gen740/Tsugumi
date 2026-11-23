{ params, inputs }:
let
  pkgs = import <nixpkgs> { };
  sdk = import <tsugumi-kit/sdk/python> { inherit pkgs; };
  common = {
    inherit inputs;
    outputs = ./_outputs.fbs;
    params = ./params.nix;
  };
in
{
  run = sdk.pythonCard (common // {
    src = ./src;
    packages = ps: [
      ps.numpy
      ps.scipy
      ps.plotly
    ];
  });
  api = sdk.pythonApi common;
  views = sdk.views;
}
