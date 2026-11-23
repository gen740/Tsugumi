{ params, inputs }:
let
  thresholdCount = inputs.results.exports.thresholdCount;
in
{
  run = ''
    jq '{out: {comparisons: (.items | length), detections: ([.items[] | select(.detected)] | length)}}' \
      "$JSON_RESULTS" > "$TSUGUMI_OUTPUT"
    jq --arg thresholds '${toString thresholdCount}' \
      '{text: "\($thresholds) 個のしきい値で比較し、\(.out.comparisons) 件のうち \(.out.detections) 件を検出しました。"}' \
      "$TSUGUMI_OUTPUT" > "$TSUGUMI_REPORT"
  '';
}
