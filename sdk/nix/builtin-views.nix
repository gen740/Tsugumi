{ pkgs }:
let
  packages = builtins.fromJSON (builtins.readFile ../views/packages.json);
  bundle = import ./view-module.nix { inherit pkgs; };
  library =
    name: file:
    pkgs.runCommand "${name}-view-library" { nativeBuildInputs = [ pkgs.esbuild ]; } ''
      mkdir -p source "$out/$(dirname ${file})"
      tar -xzf ${pkgs.fetchurl packages.${name}} --strip-components=1 -C source package/${file}
      # The view loads UMD as text; its comments must be removed before bundling.
      esbuild source/${file} --minify --legal-comments=none --outfile=$out/${file}
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
