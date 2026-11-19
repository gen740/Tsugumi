#!/usr/bin/env bash
set -euo pipefail

if [[ $(uname -s) != Linux ]]; then
  echo "The remote execution integration requires a Linux runner." >&2
  exit 2
fi

scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
nix build --cores 4 --max-jobs 1 .#workerd --out-link "$scratch/package"
# Fixtures generate Ed25519 identities and authorize hostd by its public key.
  TSUGUMI_REMOTE_TEST_DAEMON="$scratch/package/bin/tsugumi-workerd" \
  "$CORE_TEST_ENV" python3 -m pytest -q -m environment tests/core/integration/test_remote_execution.py

TSUGUMI_REMOTE_TEST_DAEMON="$scratch/package/bin/tsugumi-workerd" \
  "$CORE_TEST_ENV" python3 -m pytest -q tests/remote
