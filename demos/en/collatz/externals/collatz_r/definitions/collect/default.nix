{ params, inputs }:
let
  pkgs = import <nixpkgs> { };
  sdk = import <tsugumi-kit/sdk/r> { inherit pkgs; };
  # The pinned SDK calls stats::setNames for an empty result. Named empty
  # lists need no stats package or system shell in the embedded worker.
  sdkSource = <tsugumi-kit/sdk/r/tsugumi.R>;
  sdkRuntime = pkgs.writeText "tsugumi-r-runtime.R" (
    builtins.replaceStrings
      [ "setNames(list(), character())" ]
      [ "structure(list(), names = character())" ]
      (builtins.readFile sdkSource)
  );
  common = {
    inherit params;
    inputs = ./_inputs.fbs;
    outputs = ./_outputs.fbs;
  };
in
{
  run = (sdk.rCard (common // { src = ./src; })).overrideAttrs (old: {
    buildCommand = builtins.replaceStrings
      [ "${sdkSource}" ]
      [ (toString sdkRuntime) ]
      old.buildCommand;
  });
  api = sdk.rApi common;
  views = builtins.filter (view: view.name == "markdown") sdk.views;
}
