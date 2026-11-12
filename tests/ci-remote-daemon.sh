#!/usr/bin/env bash
set -euo pipefail

if [[ $(uname -s) != Linux ]]; then
  echo "The remote execution integration requires a Linux runner." >&2
  exit 2
fi

scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
nix build .#remote-daemon --out-link "$scratch/package"
TSUGUMI_REMOTE_TEST_DAEMON="$scratch/package/bin/tsugumi-remote-daemon" \
  "$CORE_TEST_ENV" python3 -m pytest -q -m environment tests/core/integration/test_remote_execution.py
