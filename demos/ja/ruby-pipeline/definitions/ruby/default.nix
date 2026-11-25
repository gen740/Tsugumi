{ params, inputs }:
let
  pkgs = import <nixpkgs> { };
  sdk = import <tsugumi-kit/sdk/ruby> { inherit pkgs; };
  common = {
    inherit params;
    inputs = ./_inputs.fbs;
    outputs = ./_outputs.fbs;
  };
in
{
  run = sdk.rubyCard (common // {
    src = ./src;
  });
  api = sdk.rubyApi common;
  views = builtins.filter (view: view.name == "table") sdk.views;
}
