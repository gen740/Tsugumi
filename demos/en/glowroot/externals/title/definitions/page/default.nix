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
  views = [
    (sdk.dawnView {
      name = "screen";
      src = ./views;
      # Noto Sans CJK JP at 32 px: Latin, kana and common kanji for the labels.
      font = {
        pixelSize = 32;
      };
      cflags = [
        "-Wall"
        "-Wextra"
        "-Werror"
      ];
    })
  ];
}
