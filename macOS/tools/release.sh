#!/usr/bin/env bash
# Cuts a release: sets VERSION, commits it onto origin/main, tags
# v<version> and pushes both. The release workflow
# (.github/workflows/release.yml) then builds the app and publishes it, and
# installed release builds update themselves to it.
#
#   macOS/tools/release.sh 0.2.0
#   macOS/tools/release.sh 0.2.0-rc.1
#
# Run it from a checkout whose HEAD is origin/main (any branch or worktree).
# The same can be done without a checkout:
#   gh workflow run release.yml -f version=0.2.0
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$repo"
release="${1:-}"
[[ "$release" =~ ^([0-9]+\.[0-9]+\.[0-9]+)(-rc\.([1-9][0-9]*))?$ ]] || { echo "usage: $0 <major.minor.patch[-rc.number]>" >&2; exit 2; }
version="${BASH_REMATCH[1]}"
candidate="${BASH_REMATCH[3]:-}"
tag="v$release"
[ -z "$(git status --porcelain --untracked-files=no)" ] || { echo "the working tree has changes" >&2; exit 1; }
git fetch -q --tags origin main
[ "$(git rev-parse HEAD)" = "$(git rev-parse origin/main)" ] || { echo "HEAD is not origin/main" >&2; exit 1; }
if git rev-parse -q --verify "refs/tags/$tag" >/dev/null; then
  echo "$tag exists already" >&2
  exit 1
fi
last="$(git tag -l 'v*.*.*' | sed -nE 's/^v([0-9]+\.[0-9]+\.[0-9]+)(-rc\.[1-9][0-9]*)?$/\1/p' | sort -t. -k1,1n -k2,2n -k3,3n | tail -1)"
if [ -n "$last" ] && [ "$(printf '%s\n%s\n' "$last" "$version" | sort -t. -k1,1n -k2,2n -k3,3n | tail -1)" != "$version" ]; then
  echo "$tag is older than v$last" >&2
  exit 1
fi
if [ -n "$candidate" ]; then
  if git rev-parse -q --verify "refs/tags/v$version" >/dev/null; then
    echo "v$version has already been tagged" >&2
    exit 1
  fi
  last_candidate="$(git tag -l "v$version-rc.*" | sed -nE 's/^.*-rc\.([1-9][0-9]*)$/\1/p' | sort -n | tail -1)"
  if [ "$candidate" -le "${last_candidate:-0}" ]; then
    echo "$tag is older than v$version-rc.$last_candidate" >&2
    exit 1
  fi
fi
if [ "$(cat VERSION)" != "$version" ]; then
  echo "$version" > VERSION
  git commit -qm "Release $release" VERSION
fi
git tag -a "$tag" -m "Tsugumi $release"
git push --atomic origin HEAD:refs/heads/main "refs/tags/$tag"
echo "$tag pushed: https://github.com/gen740/Tsugumi/actions/workflows/release.yml"
