{ pkgs, inputs, params }:
let
  sdk = inputs.tsugumi.sdk { languages = [ "cpp" ]; };
  package = inputs.tsugumi.cppCard { inherit sdk; src = ./src; };
in
{
  inherit package;
  run = "${package}/lib/main.so";
  reuseWorker = true;
}
