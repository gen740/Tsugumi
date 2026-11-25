#!/usr/bin/env bash
# The checks CI runs first and cheaply: the static tests (tests/static, which
# read source files and the pinned demos) and the Nix Web typecheck and behavior
# checks, including the demo player. Run it before every push.
#
#   tests/check-fast.sh            static tests, then the Web Nix checks
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
TSUGUMI_DEMOS_DIR=$(nix "${nix_flags[@]}" build --no-link --print-out-paths .#demos)
export TSUGUMI_DEMOS_DIR
# The same pytest as ci.yml's Static checks job.
nix "${nix_flags[@]}" shell --inputs-from . nixpkgs#python3Packages.pytest -c \
  python3 tests/run-isolated.py pytest -q -m static -p no:cacheprovider tests/static

if $run_nix; then
  system=$(nix "${nix_flags[@]}" eval --raw --impure --expr builtins.currentSystem)
  step "Web UI typecheck and behavior contracts"
  nix "${nix_flags[@]}" build --no-link ".#checks.$system.web-typecheck" ".#checks.$system.web"
fi
step "Fast checks passed"
