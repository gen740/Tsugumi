{ pkgs, inputs, params }:
{
  package = inputs.tsugumi.cppCard { src = ./src; };
  reuseWorker = true;
}
