{ params, inputs }:
# Import the CIAO recipe from the project lock and build it with the project nixpkgs.
let
  pkgs = import <nixpkgs> { };
  ciao = import <ciao/src/package.nix> { inherit pkgs; };
  sdk = import <tsugumi-kit/sdk/python> { inherit pkgs; };
  common = {
    params = { version = (builtins.parseDrvName ciao.name).version; };
    inputs = ./_inputs.fbs;
    outputs = ./_outputs.fbs;
  };
in
{
  run = sdk.pythonCard (common // { src = ./src; });
  api = sdk.pythonApi common;
  exports.ciao = ciao;
}
