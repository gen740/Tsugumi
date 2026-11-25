{ params, inputs }:
let
  pkgs = import <nixpkgs> { };
  sdk = import <tsugumi-kit/sdk/csharp> { inherit pkgs; };
  common = {
    inherit params;
    inputs = ./_inputs.fbs;
    outputs = ./_outputs.fbs;
  };
in
{
  run = sdk.csharpCard (common // {
    src = ./src;
    # Json.NET, as NuGet locked it, and the NAR hash of the packages the lock
    # names (docs/CSharp.md, "NuGet packages").
    lock = ./packages.lock.json;
    hash = "sha256-wU+UzEeC0HVXwrqFmgW6FE9Li6S0XxQFzuizoaVoVGY=";
  });
  api = sdk.csharpApi common;
  views = builtins.filter (view: view.name == "line-chart") sdk.views;
}
