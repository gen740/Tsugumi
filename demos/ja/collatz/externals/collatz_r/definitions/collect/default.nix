{ params, inputs }:
let
  pkgs = import <nixpkgs> { };
  sdk = import <tsugumi-kit/sdk/r> { inherit pkgs; };
  common = {
    inherit params;
    inputs = ./_inputs.fbs;
    outputs = ./_outputs.fbs;
  };
in
{
  run = sdk.rCard (common // {
    src = ./src;
  });
  api = sdk.rApi common;
  views = builtins.filter (view: view.name == "markdown") sdk.views;
}
