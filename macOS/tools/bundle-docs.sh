#!/usr/bin/env bash
# Bundles the Astro story pages and their local assets for Help > Learn about Tsugumi.
# The site must already have been built with npm --prefix docs/site run build.
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
site="$repo/build/docs"
dest="${1:?usage: bundle-docs.sh <destination>}"
mkdir -p "$dest/pages" "$dest/site/js" "$dest/site/assets" "$dest/assets"
stories=()
for page in "$site"/pages/*.html; do
  if grep -q 'class="story"' "$page"; then stories+=("$page"); fi
done
[ "${#stories[@]}" -gt 0 ] || { echo "no story pages in $site/pages; build the docs first" >&2; exit 1; }
cp "${stories[@]}" "$dest/pages/"
cp "$site/site/site.css" "$dest/site/"
cp "$site"/site/js/{reader,art,scenes,scenes2}.js "$dest/site/js/"
cp "$site"/site/assets/* "$dest/site/assets/"
# Only the images actually named by the story pages, rather than the demo gallery.
while IFS= read -r used; do
  mkdir -p "$dest/$(dirname "$used")"
  cp "$site/$used" "$dest/$used"
done < <(grep -oh 'src="\.\./assets/[^"]*"\|href="\.\./assets/[^"]*"' "${stories[@]}" | sed 's/.*"\.\.\/\(.*\)"/\1/' | sort -u)
