{ pkgs, inputs, params }:
{
  run = inputs.tsugumi.reportCard {
    src = ./src;
    entry = "report.qmd";
    packages = ps: [ ];
  };
}
