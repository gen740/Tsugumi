{ pkgs }:
let
  packages = builtins.fromJSON (builtins.readFile ../views/packages.json);
  bundle = import ./view-module.nix { inherit pkgs; };
  library =
    name: file:
    pkgs.runCommand "${name}-view-library" { } ''
      mkdir -p $out
      tar -xzf ${pkgs.fetchurl packages.${name}} --strip-components=1 -C $out package/${file} package/LICENSE
    '';
  plotly = library "plotly.js-dist-min" "plotly.min.js";
  vega = library "vega" "build/vega.min.js";
  lite = library "vega-lite" "build/vega-lite.min.js";
  embed = library "vega-embed" "build/vega-embed.min.js";
  source = pkgs.runCommand "tsugumi-builtin-view-source" { } ''
    mkdir -p $out
    cp ${../views}/*.js $out/
    cp ${plotly}/plotly.min.js $out/plotly.umd
    cp ${vega}/build/vega.min.js $out/vega.umd
    cp ${lite}/build/vega-lite.min.js $out/vega-lite.umd
    cp ${embed}/build/vega-embed.min.js $out/vega-embed.umd
  '';
in
{
  line-chart = bundle {
    src = ../views;
    entry = "line-chart.js";
    name = "tsugumi-view-line-chart";
  };
  plotly = bundle {
    src = source;
    entry = "plotly.js";
    name = "tsugumi-view-plotly";
  };
  vega-lite = bundle {
    src = source;
    entry = "vega-lite.js";
    name = "tsugumi-view-vega-lite";
  };
}
