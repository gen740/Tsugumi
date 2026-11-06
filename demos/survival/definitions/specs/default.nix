{ pkgs, inputs, params }:
{
  run = inputs.tsugumi.rCard { src = ./src; packages = ps: [ ps.survival ps.ggplot2 ]; };
}
