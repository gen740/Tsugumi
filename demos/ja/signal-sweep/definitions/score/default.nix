{ params, inputs }:
let
  pkgs = import <nixpkgs> { };
  sdk = import <tsugumi-kit/sdk/python> { inherit pkgs; };
  common = {
    inputs = ./_inputs.fbs;
    outputs = ./_outputs.fbs;
  };
  thresholds = [ 10 14 ];
  arguments = threshold: common // { params = { inherit threshold; }; };
in
{
  run = map (threshold: {
    label = "threshold=${toString threshold}";
    run = sdk.pythonCard (arguments threshold // { src = ./src; });
  }) thresholds;
  api = sdk.pythonApi (arguments (builtins.head thresholds));
}
