{ params, inputs }:
let
  pkgs = import <nixpkgs> { };
  sdk = import <tsugumi-kit/sdk/python> { inherit pkgs; };
  common = {
    params = params // {
      image_file = toString inputs.mitosis.image;
      image_url = inputs.mitosis.url;
    };
    inputs = ./_inputs.fbs;
    outputs = ./_outputs.fbs;
  };
in
{
  run = sdk.pythonCard (common // {
    src = ./src;
    packages = ps: [
      ps.numpy
      ps.scipy
      ps.scikit-image
      ps.matplotlib
    ];
  });
  api = sdk.pythonApi common;
  # TODO(open): Port the demo display.
}
