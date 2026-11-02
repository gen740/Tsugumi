{ pkgs, inputs, params }:
let
  sdk = inputs.tsugumi.sdk { languages = [ "python" ]; };
  package = inputs.tsugumi.pythonCard { inherit sdk; src = ./src; };
in
{
  inherit package;
  run = "${package}/lib/main.so";
}
