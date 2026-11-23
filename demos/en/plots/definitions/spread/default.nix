{ params, inputs }:
let
  pkgs = import <nixpkgs> { };
  sdk = import <tsugumi-kit/sdk/python> { inherit pkgs; };
  common = {
    inherit params;
    inputs = if builtins.pathExists ./_inputs.fbs then ./_inputs.fbs else null;
    outputs = ./_outputs.fbs;
  };
in
{
  run = sdk.pythonCard (common // {
    src = ./src;
    packages = ps: [
      ps.numpy
      ps.altair
    ];
  });
  api = sdk.pythonApi common;
  # TODO(open): Port the demo display.
}
