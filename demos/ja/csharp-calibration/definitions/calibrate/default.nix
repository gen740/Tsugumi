{ pkgs, inputs, params }:
let sdk = inputs.tsugumi.sdk { languages = [ "csharp" ]; };
in {
  run = inputs.tsugumi.csharpCard {
    inherit sdk;
    src = ./src;
    # Json.NET, as NuGet locked it, and the NAR hash of the packages the lock
    # names (docs/CSharp.md, "NuGet packages").
    lock = ./packages.lock.json;
    hash = "sha256-wU+UzEeC0HVXwrqFmgW6FE9Li6S0XxQFzuizoaVoVGY=";
  };
}
