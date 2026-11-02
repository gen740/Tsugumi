{ pkgs, inputs, params }:
{
  # A shell-only Card: no package. $DIR_REPORT is the input directory and the
  # output port values are written as JSON to $TSUGUMI_OUTPUT.
  script = ''
    dest="$TSUGUMI_OUTPUT_DIR/archive"
    mkdir -p "$dest"
    cp -R "$DIR_REPORT/." "$dest/"
    date -u +%Y-%m-%dT%H:%M:%SZ > "$dest/ARCHIVED_AT"
    echo "archived $(ls "$dest" | wc -l) files"
    printf '{"archive": {"value": "%s"}}' "$dest" > "$TSUGUMI_OUTPUT"
  '';
}
