{ params, inputs }:
let
  pkgs = import <nixpkgs> { };
  sdk = import <tsugumi-kit/sdk/input> { inherit pkgs; };
in
{
  run = map (value: {
    label = "value=${toString value}";
    run = sdk.input {
      outputs = ./_outputs.fbs;
      values.out = { inherit value; };
    };
  }) params.values;
}
