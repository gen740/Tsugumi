{ params, inputs }:
let
  pkgs = import <nixpkgs> { };
  sdk = import <tsugumi-kit/sdk/python> { inherit pkgs; };
  common = {
    inputs = ./_inputs.fbs;
    outputs = ./_outputs.fbs;
  };
  arguments = signal: threshold: common // { params = { inherit signal threshold; }; };
in
{
  run = pkgs.lib.concatMap (
    signal:
    map (threshold: {
      label = "value=${toString signal}, threshold=${toString threshold}";
      run = sdk.pythonCard (arguments signal threshold // { src = ./src; });
    }) params.thresholds
  ) params.values;
  api = sdk.pythonApi (arguments 0 0);
}
