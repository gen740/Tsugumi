# The bilingual demo projects and reference files pinned by the flake.
{ runCommand, tsugumi-demos }:
runCommand "tsugumi-demos" { } ''
  mkdir -p "$out"
  cp -R ${tsugumi-demos}/. "$out/"
''
