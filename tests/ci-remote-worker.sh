#!/usr/bin/env bash
set -euo pipefail

if [[ $(uname -s) != Linux ]]; then
  echo "The remote execution integration requires a Linux runner." >&2
  exit 2
fi

scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
nix build --cores 4 --max-jobs 1 .#workerd --out-link "$scratch/package"
# Seed the private remote stores with what every remote test Card shares: the
# plan's worker for this system, and a Python with flatbuffers from the pinned
# nixpkgs, which the test Cards' Python closure usually shares. Nothing here
# names a kit product: each daemon still builds its Cards' own paths.
echo "Preparing the shared remote Card closure"
time TSUGUMI_NIX_DIR="$(readlink -f "$(dirname "$CORE_TEST_ENV")/../share/tsugumi/nix")" \
  nix build --cores 4 --max-jobs 1 --impure --out-link "$scratch/seed-manifest" --expr '
    let
      nixpkgs = (builtins.getFlake (toString ./.)).inputs.nixpkgs;
      pkgs = import nixpkgs {
        system = builtins.currentSystem;
        config = { };
        overlays = [ ];
      };
      nixDir = /. + builtins.getEnv "TSUGUMI_NIX_DIR";
    in
    pkgs.linkFarm "tsugumi-remote-test-seed" [
      { name = "worker"; path = import (nixDir + "/worker/worker.nix") { inherit pkgs; }; }
      { name = "python"; path = pkgs.python3.withPackages (ps: [ ps.flatbuffers ]); }
    ]
  '
export TSUGUMI_REMOTE_TEST_SEED_MANIFEST="$scratch/seed-manifest"

# Fixtures generate Ed25519 identities and authorize hostd by its public key.
TSUGUMI_REMOTE_TEST_DAEMON="$scratch/package/bin/tsugumi-workerd" \
  "$CORE_TEST_ENV" python3 -m pytest -vv -o faulthandler_timeout=180 --durations=0 -m environment tests/core/integration/test_remote_execution.py

TSUGUMI_REMOTE_TEST_DAEMON="$scratch/package/bin/tsugumi-workerd" \
  "$CORE_TEST_ENV" python3 -m pytest -vv -o faulthandler_timeout=180 --durations=0 tests/remote
