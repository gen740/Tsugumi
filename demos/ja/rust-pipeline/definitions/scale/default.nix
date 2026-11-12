{ pkgs, inputs, params }:
let sdk = inputs.tsugumi.sdk { languages = [ "rust" ]; };
in { run = inputs.tsugumi.rustCard { inherit sdk; src = ./src; }; }
