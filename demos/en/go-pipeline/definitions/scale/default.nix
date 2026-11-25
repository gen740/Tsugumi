{ params, inputs }:
let
  pkgs = import <nixpkgs> { };
  sdk = import <tsugumi-kit/sdk/go> { inherit pkgs; };
  common = {
    inherit params;
    inputs = ./_inputs.fbs;
    outputs = ./_outputs.fbs;
  };
in
{
  run = sdk.goCard (common // {
    src = ./src;
  });
  api = sdk.goApi common;
  views = builtins.filter (view: view.name == "markdown") sdk.views;
}
