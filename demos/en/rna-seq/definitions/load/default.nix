{ pkgs, inputs, params }:
{
  run = inputs.tsugumi.rCard { src = ./src; packages = ps: [ ps.airway ps.SummarizedExperiment ]; };
}
