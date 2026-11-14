{
  pkgs,
  inputs,
  params,
}:
let
  thresholdCount = inputs.results.thresholdCount;
in
{
  run = ''
    jq '{out: {comparisons: (.items | length), detections: ([.items[] | select(.detected)] | length)}}' \
      "$JSON_RESULTS" > "$TSUGUMI_OUTPUT"
    jq --arg thresholds '${toString thresholdCount}' \
      '{text: "\(.out.detections) of \(.out.comparisons) comparisons passed across \($thresholds) thresholds."}' \
      "$TSUGUMI_OUTPUT" > "$TSUGUMI_REPORT"
  '';
}
