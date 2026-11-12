{ pkgs, inputs, params }:
let sdk = inputs.tsugumi.sdk { languages = [ "csharp" ]; };
in { run = inputs.tsugumi.csharpCard { inherit sdk; packages = [{ name = "Newtonsoft.Json"; version = "13.0.3"; }]; nugetDeps = ./nuget-deps.json; src = ./src; }; }
