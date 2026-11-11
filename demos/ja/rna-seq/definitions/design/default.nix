{ pkgs, inputs, params }:
{
  run = inputs.tsugumi.rCard { src = ./src; packages = ps: [ ps.DESeq2 ps.ggplot2 ]; };
}
