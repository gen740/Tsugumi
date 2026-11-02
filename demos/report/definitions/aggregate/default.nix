{ pkgs, inputs, params }:
{
  package = inputs.tsugumi.pythonCard { src = ./src; };
}
