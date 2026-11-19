{
  pkgs,
  inputs,
  params,
}:
{
  run = inputs.tsugumi.rubyCard {
    src = ./src;
  };
}
