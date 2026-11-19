#!/usr/bin/env bash
set -euo pipefail

if [[ $(uname -s) != Linux ]]; then
  echo "The remote execution integration requires a Linux runner." >&2
  exit 2
fi

scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
nix build --cores 4 --max-jobs 1 .#workerd --out-link "$scratch/package"
# Build just the shared Bash runtime, SDK worker and built-in views. The
# deadline case uses the worker output without pulling in the core package.
echo "Preparing the shared remote Card closure"
time TSUGUMI_SDK_DIR="$(readlink -f "$(dirname "$CORE_TEST_ENV")/../share/tsugumi/sdk")" \
  nix build --cores 4 --max-jobs 1 --impure --out-link "$scratch/seed-manifest" --expr '
    let
      nixpkgs = (builtins.getFlake (toString ./.)).inputs.nixpkgs;
      pkgs = import nixpkgs {
        system = builtins.currentSystem;
        config = { };
        overlays = [ ];
      };
      sdk = import (/. + builtins.getEnv "TSUGUMI_SDK_DIR" + "/nix") {
        inherit pkgs;
        generated = null;
      };
    in
    sdk.manifestFor true { run = ":"; }
  '
export TSUGUMI_REMOTE_TEST_SEED_MANIFEST="$scratch/seed-manifest"

# Fixtures generate Ed25519 identities and authorize hostd by its public key.
TSUGUMI_REMOTE_TEST_DAEMON="$scratch/package/bin/tsugumi-workerd" \
  "$CORE_TEST_ENV" python3 -m pytest -q --durations=0 -m environment tests/core/integration/test_remote_execution.py

TSUGUMI_REMOTE_TEST_DAEMON="$scratch/package/bin/tsugumi-workerd" \
  "$CORE_TEST_ENV" python3 -m pytest -q --durations=0 tests/remote
