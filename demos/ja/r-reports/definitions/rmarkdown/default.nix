{ pkgs, inputs, params }:
{
  run = inputs.tsugumi.reportCard {
    src = ./src;
    entry = "report.Rmd";
    packages = ps: [ ];
  };
}
