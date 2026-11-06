{ pkgs, inputs, params }: {
  run = inputs.tsugumi.rCard { src = ./src; };
}
