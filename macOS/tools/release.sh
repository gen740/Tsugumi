#!/usr/bin/env bash
# Cuts a release: sets VERSION, commits it onto origin/main, tags
# v<version> and pushes both. The release workflow
# (.github/workflows/release.yml) then builds the app and publishes it, and
# installed release builds update themselves to it.
#
#   macOS/tools/release.sh 0.2.0
#
# Run it from a checkout whose HEAD is origin/main (any branch or worktree).
# The same can be done without a checkout:
#   gh workflow run release.yml -f version=0.2.0
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$repo"
version="${1:-}"
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { sed -n '7,7s/^# \{0,1\}//p' "$0" >&2; exit 2; }
[ -z "$(git status --porcelain --untracked-files=no)" ] || { echo "the working tree has changes" >&2; exit 1; }
git fetch -q --tags origin main
[ "$(git rev-parse HEAD)" = "$(git rev-parse origin/main)" ] || { echo "HEAD is not origin/main" >&2; exit 1; }
if git rev-parse -q --verify "refs/tags/v$version" >/dev/null; then
  echo "v$version exists already" >&2
  exit 1
fi
last="$(git tag -l 'v*.*.*' | sed 's/^v//' | sort -t. -k1,1n -k2,2n -k3,3n | tail -1)"
if [ -n "$last" ] && [ "$(printf '%s\n%s\n' "$last" "$version" | sort -t. -k1,1n -k2,2n -k3,3n | tail -1)" != "$version" ]; then
  echo "v$version is not newer than v$last" >&2
  exit 1
fi
if [ "$(cat VERSION)" != "$version" ]; then
  echo "$version" > VERSION
  git commit -qm "Release $version" VERSION
fi
git tag -a "v$version" -m "Tsugumi $version"
git push origin HEAD:refs/heads/main "refs/tags/v$version"
echo "v$version pushed: https://github.com/gen740/Tsugumi/actions/workflows/release.yml"
