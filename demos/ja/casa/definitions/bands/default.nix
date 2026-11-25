{ params, inputs }:
let
  pkgs = import <nixpkgs> { };
  sdk = import <tsugumi-kit/sdk/python> { inherit pkgs; };
  common = {
    params = params // {
      ciao_root = toString inputs.ciao.ciao;
      events_file = toString inputs.load.events;
      inherit (inputs.load) obsid;
      events_url = inputs.load.url;
      events_hash = inputs.load.hash;
    };
    inputs = ./_inputs.fbs;
    outputs = ./_outputs.fbs;
  };
in
{
  run = sdk.pythonCard (common // {
    src = ./src;
    tools = [ inputs.ciao.ciao ];
    packages = ps: [
      ps.numpy
      ps.astropy
    ];
  });
  api = sdk.pythonApi common;
  views = builtins.filter (view: view.name == "markdown") sdk.views;
}
