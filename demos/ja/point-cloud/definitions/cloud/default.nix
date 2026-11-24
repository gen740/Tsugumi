{ params, inputs }:
let
  pkgs = import <nixpkgs> { };
  sdk = import <tsugumi-kit/sdk/cpp> { inherit pkgs; };
  common = {
    inherit params;
    inputs = ./_inputs.fbs;
    outputs = ./_outputs.fbs;
  };
in
{
  run = sdk.cppCard (common // { src = ./src; });
  api = sdk.cppApi common;
  # The bundled HTML scatter plot also uses the name "points".
  views = [
    (sdk.dawnView {
      name = "points";
      src = ./views;
      cflags = [
        "-Wall"
        "-Wextra"
        "-Werror"
      ];
    })
  ];
}
