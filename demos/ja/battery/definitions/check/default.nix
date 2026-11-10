{ pkgs, inputs, params }: {
  run = inputs.tsugumi.juliaCard { src = ./src; };
}
