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
    packages = ps: [
      ps.metafor
      ps.ggplot2
    ];
  });
  api = sdk.rApi common;
  # TODO(open): Port the demo display.
}
