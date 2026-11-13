{
  pkgs,
  inputs,
  params,
}:
let
  sdk = inputs.tsugumi.sdk { languages = [ "python" ]; };
in
{
  views.points = inputs.tsugumi.wasmViewModule {
    src = ./view;
    name = "point-cloud-wasm-view";
  };
  run = inputs.tsugumi.pythonCard {
    inherit sdk;
    src = ./src;
  };
}
