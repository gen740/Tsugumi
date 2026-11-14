{ pkgs, inputs, params }:
{
  run = ''
    set -e
    if [[ ! -r "$JSON_IN" ]]; then
      printf '%s\n' 'Missing input: in' >&2
      exit 1
    fi

    cat "$JSON_IN"

    # Keep the Canvas preview below the worker frame limit. jq slices at
    # Unicode character boundaries; stdout still receives the full value.
    preview='def preview: if utf8bytelength > 1048576 then
      .[0:((1048576 - 64) / 4)] + "\n\n[Display truncated.]"
      else . end;'
    case ${pkgs.lib.escapeShellArg params.format} in
      text)
        jq -Rs "$preview"' {text: preview}' "$JSON_IN" > "$TSUGUMI_REPORT"
        ;;
      markdown)
        jq -es --arg field ${pkgs.lib.escapeShellArg params.markdown_field} "$preview"'
          if length == 1 and (.[0] | type == "object" and (.[$field] | type == "string"))
          then (.[0][$field] |
            if contains("\u0000") then error("Markdown input contains a NUL character")
            else {markdown: preview}
            end)
          else error("Markdown input must contain the string field " + $field)
          end
        ' "$JSON_IN" > "$TSUGUMI_REPORT"
        ;;
      *)
        printf '%s\n' 'Unknown display format: expected text or markdown' >&2
        exit 1
        ;;
    esac
  '';
}
