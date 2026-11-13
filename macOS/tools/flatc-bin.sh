#!/usr/bin/env bash
# Prints the folder of flatc from the flake's nixpkgs (flake.lock), for the
# Swift builds: they use Apple's toolchain in a clean environment and take only
# flatc from Nix.
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
echo "$(nix build --no-link --print-out-paths --inputs-from "$repo" nixpkgs#flatbuffers)/bin"
