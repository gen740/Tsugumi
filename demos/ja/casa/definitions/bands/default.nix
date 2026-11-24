{ params, inputs }:
let
  pkgs = import <nixpkgs> { };
  sdk = import <tsugumi-kit/sdk/python> { inherit pkgs; };
  common = {
    params = params // { ciao_root = toString inputs.ciao.ciao; };
    inputs = ./_inputs.fbs;
    outputs = ./_outputs.fbs;
  };
in
{
  run = sdk.pythonCard (common // {
    src = ./src;
    tools = [ inputs.ciao.ciao ];
    packages = ps: [
      ps.numpy
      ps.astropy
    ];
  });
  api = sdk.pythonApi common;
  # TODO(open): Port the demo display.
}
