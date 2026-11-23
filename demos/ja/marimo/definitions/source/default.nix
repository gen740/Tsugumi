{ params, inputs }:
{
  run = ''
    jq '{value: {source: .measurement}}' \
      <<< "$TSUGUMI_PARAMS_JSON" > "$TSUGUMI_OUTPUT"
  '';
}
