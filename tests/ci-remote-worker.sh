#!/usr/bin/env bash
set -euo pipefail

if [[ $(uname -s) != Linux ]]; then
  echo "The remote execution integration requires a Linux runner." >&2
  exit 2
fi

scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
nix build --cores 4 --max-jobs 1 .#workerd --out-link "$scratch/package"
nix build --cores 4 --max-jobs 1 .#tsugumid --out-link "$scratch/core"
# Fixtures generate Ed25519 identities and authorize hostd by its public key.
TSUGUMI_HOSTD="$scratch/core/bin/tsugumi-hostd" \
  TSUGUMI_REMOTE_TEST_DAEMON="$scratch/package/bin/tsugumi-workerd" \
  "$CORE_TEST_ENV" python3 -m pytest -q --durations=0 -m environment tests/core/integration/test_remote_execution.py

TSUGUMI_REMOTE_TEST_DAEMON="$scratch/package/bin/tsugumi-workerd" \
  "$CORE_TEST_ENV" python3 -m pytest -q --durations=0 tests/remote
