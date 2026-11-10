{ pkgs, inputs, params }:
{
  run = inputs.tsugumi.rCard { src = ./src; packages = ps: [ ps.metafor ps.ggplot2 ps.ggrepel ]; };
}
