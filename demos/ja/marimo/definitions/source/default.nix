{ pkgs, inputs, params }:
{
  run = ''
    jq '{value: {source: .measurement}}' \
      <<< "$TSUGUMI_PARAMS_JSON" > "$TSUGUMI_OUTPUT"
  '';
}
