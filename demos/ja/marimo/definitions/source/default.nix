{ pkgs, inputs, params }:
{
  run = ''
    jq '{value: {source: .measurement, result: (.measurement * .factor)}}' \
      <<< "$TSUGUMI_PARAMS_JSON" > "$TSUGUMI_OUTPUT"
  '';
}
