#!/usr/bin/env bash
# The checks CI runs first and cheaply: the static tests (tests/static, which
# read only files of the repository) and the Nix check that builds the Web UI
# contract, including the demo player. Run it before every push.
#
#   tests/check-fast.sh            static tests, then the Web Nix check
#   tests/check-fast.sh --no-nix   only the static tests (a few seconds)
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
nix_flags=(--extra-experimental-features 'nix-command flakes')
run_nix=true
case "${1:-}" in
  "") ;;
  --no-nix) run_nix=false ;;
  *) echo "usage: $0 [--no-nix]" >&2; exit 2 ;;
esac

step() { printf '\n== %s\n' "$1"; }

step "Static tests"
# The same pytest as ci.yml's Static checks job.
nix "${nix_flags[@]}" shell --inputs-from . nixpkgs#python3Packages.pytest -c \
  pytest -q -m static -p no:cacheprovider tests/static

if $run_nix; then
  system=$(nix "${nix_flags[@]}" eval --raw --impure --expr builtins.currentSystem)
  step "Web UI typecheck and behavior contracts"
  nix "${nix_flags[@]}" build --no-link ".#checks.$system.web"
fi
step "Fast checks passed"
