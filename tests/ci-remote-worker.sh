#!/usr/bin/env bash
set -euo pipefail

if [[ $(uname -s) != Linux ]]; then
  echo "The remote execution integration requires a Linux runner." >&2
  exit 2
fi

scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
nix build .#remote-worker --out-link "$scratch/package"
nix build .#web --out-link "$scratch/web"
TSUGUMI_EXECUTION_BROKER_MODULE="$scratch/web/share/tsugumi/web/dist/execution-broker.js" \
  TSUGUMI_REMOTE_TEST_DAEMON="$scratch/package/bin/tsugumi-remote-worker" \
  "$CORE_TEST_ENV" python3 -m pytest -q -m environment tests/core/integration/test_remote_execution.py

TSUGUMI_REMOTE_TEST_DAEMON="$scratch/package/bin/tsugumi-remote-worker" \
  "$CORE_TEST_ENV" python3 -m pytest -q tests/remote
