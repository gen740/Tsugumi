{ params, inputs }:
let
  pkgs = import <nixpkgs> { };
  sdk = import <tsugumi-kit/sdk/python> { inherit pkgs; };
  common = {
    params = params // { events_file = toString inputs.load.events; };
    inputs = ./_inputs.fbs;
    outputs = ./_outputs.fbs;
  };
in
{
  run = sdk.pythonCard (common // {
    src = ./src;
    packages = ps: [
      ps.numpy
      ps.astropy
      ps.matplotlib
    ];
  });
  api = sdk.pythonApi common;
  views = builtins.filter (view: view.name == "image") sdk.views;
}
