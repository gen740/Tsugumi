{ params, inputs }:
let
  thresholds = [
    10
    14
  ];
  # Each entry is one implementation; its label names it in the history and in
  # a comparison of runs.
  compare = threshold: {
    label = "threshold=${toString threshold}";
    run = ''
      signal=$(jq -r '.value' "$JSON_SIGNAL")
      jq -nc --argjson signal "$signal" --argjson threshold '${toString threshold}' \
        '{out: {$signal, $threshold, detected: ($signal >= $threshold)}}' > "$TSUGUMI_OUTPUT"
    '';
  };
in
{
  run = map compare thresholds;
  exports.thresholdCount = builtins.length thresholds;
}
