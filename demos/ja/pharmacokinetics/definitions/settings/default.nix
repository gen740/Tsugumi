{ params, inputs }:
let
  pkgs = import <nixpkgs> { };
  sdk = import <tsugumi-kit/sdk/input> { inherit pkgs; };
in
{
  run = sdk.input {
    outputs = ./_outputs.fbs;
    values.settings = params;
  };
}
