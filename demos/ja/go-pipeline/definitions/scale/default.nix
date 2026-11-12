{ pkgs, inputs, params }:
let sdk = inputs.tsugumi.sdk { languages = [ "go" ]; };
in { run = inputs.tsugumi.goCard { inherit sdk; src = ./src; }; }
