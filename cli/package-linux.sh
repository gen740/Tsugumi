#!/usr/bin/env bash
# Builds dist/tsugumi-linux.tar.gz and its checksum, what cli/install.sh installs
# on Linux: the committed source the flake's `cli` package builds from (the same
# for x86_64 and aarch64; everything it fetches is pinned by flake.lock and
# web/package-lock.json).
#
#   cli/package-linux.sh
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
dist="$repo/dist"
mkdir -p "$dist"
# What nix/cli.nix and nix/core.nix read, and the flake itself.
git -C "$repo" archive --format=tar.gz --prefix=tsugumi/ -o "$dist/tsugumi-linux.tar.gz" HEAD \
  flake.nix flake.lock VERSION LICENSE THIRD_PARTY_NOTICES.md CMakeLists.txt cmake core client sdk workerd protocol nix tests/core \
  web agent ui/modules demos docs/CardGuide.md docs/site/player docs/site/player-data/assets docs/assets/tsugumi-icon.svg macOS/Container/vm-system.nix
(cd "$dist" && shasum -a 256 tsugumi-linux.tar.gz > tsugumi-linux.tar.gz.sha256)
ls -l "$dist/tsugumi-linux.tar.gz"
