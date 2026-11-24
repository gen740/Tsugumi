{ params, inputs }:
let
  pkgs = import <nixpkgs> { };
  sdk = import <tsugumi-kit/sdk/python> { inherit pkgs; };
  common = {
    inherit params;
    inputs = ./_inputs.fbs;
    outputs = ./_outputs.fbs;
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
  views = builtins.filter (view: view.name == "plotly") sdk.views;
}
