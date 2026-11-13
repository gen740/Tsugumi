{
  pkgs,
  inputs,
  params,
}:
let
  sdk = inputs.tsugumi.sdk { languages = [ "python" ]; };
in
{
  views.points = inputs.tsugumi.viewModule {
    src = ./view;
    name = "point-cloud-view";
  };
  run = inputs.tsugumi.pythonCard {
    inherit sdk;
    src = ./src;
  };
}
