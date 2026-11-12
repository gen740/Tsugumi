{ pkgs, inputs, params }:
let sdk = inputs.tsugumi.sdk { languages = [ "ruby" ]; };
in { run = inputs.tsugumi.rubyCard { inherit sdk; src = ./src; }; }
