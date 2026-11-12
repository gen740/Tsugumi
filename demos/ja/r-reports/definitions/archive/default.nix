{ pkgs, inputs, params }: { run = ''
  test -s "$DIR_RMARKDOWN/index.html"
  test -s "$DIR_QUARTO/index.html"
  printf '%s' '{"text":"Both HTML reports are saved. Open each report Card’s Artifacts to preview the table and figure."}' > "$TSUGUMI_DISPLAY"
''; }
