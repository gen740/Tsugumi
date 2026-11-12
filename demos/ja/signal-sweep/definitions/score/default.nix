{
  pkgs,
  inputs,
  params,
}:
let
  thresholds = inputs.signal.thresholds;
  compare = threshold: ''
    signal=$(jq -r '.value' "$JSON_SIGNAL")
    jq -nc --argjson signal "$signal" --argjson threshold '${toString threshold}' \
      '{out: {$signal, $threshold, detected: ($signal >= $threshold)}}' > "$TSUGUMI_OUTPUT"
  '';
in
{
  run = map compare thresholds;
  exports.thresholdCount = builtins.length thresholds;
}
