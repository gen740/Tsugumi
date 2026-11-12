{
  pkgs,
  inputs,
  params,
}:
let
  sample = signal: ''
    printf '{"out":{"value":%s}}' '${toString signal}' > "$TSUGUMI_OUTPUT"
  '';
in
{
  run = map sample [
    8
    12
    16
  ];
  exports.thresholds = [
    10
    14
  ];
}
