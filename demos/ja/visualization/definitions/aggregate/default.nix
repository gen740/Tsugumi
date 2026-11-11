{
  pkgs,
  inputs,
  params,
}:
let
  sdk = inputs.tsugumi.sdk { languages = [ "python" ]; };
in
{
  run = inputs.tsugumi.pythonCard {
    inherit sdk;
    src = ./src;
  };
}
