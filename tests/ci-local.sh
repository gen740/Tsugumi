#!/usr/bin/env bash
# Runs the checks of .github/workflows/ci.yml on this machine, as a local
# stand-in for GitHub Actions.
#
#   tests/ci-local.sh          both jobs (on a Mac: macOS here, Linux in Docker)
#   tests/ci-local.sh mac      the macOS job: Swift tests and Seatbelt isolation
#   tests/ci-local.sh linux    the Linux job: tsugumid, C++, Web UI, the tsugumi
#                              command, the docs player and the Python tests
#
# On a Mac the Linux job runs in a container (Docker or OrbStack, privileged
# so that bubblewrap can isolate Cards) on a copy of the tracked files as they
# are in the working tree, so uncommitted edits are checked too and this
# checkout's build/ and node_modules are left alone. The container's /nix is a
# named volume (tsugumi-ci-nix), kept between runs. It differs from CI in the
# CPU (an Apple silicon Mac gives arm64 Linux, CI x86_64) and, for the macOS
# job, in the macOS and Xcode of this Mac and its existing build folders.
# Keep the steps in step with ci.yml.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
# Apply the same caches to Nix commands and the embedded evaluator in this run.
export NIX_CONFIG="${NIX_CONFIG:-}
extra-substituters = https://tsugumi.cachix.org
extra-trusted-public-keys = tsugumi.cachix.org-1:am26Q4VFWYLyJ+t+O5j03MmMVmZ2Ifw6cbNUoLhlrhI="
step() { printf '\n== %s\n' "$1"; }
core_run() { "$root/build-ci/bin/tsugumi-test-env" "$@"; }
core_checks() {
  core_run "$root/build-ci/libexec/tsugumi-tests/tsugumi_agent_rpc_tests"
  core_run "$root/build-ci/libexec/tsugumi-tests/tsugumi_rpc_cli_tests"
  core_run python3 -m pytest -q --core-report -m 'not environment' tests/core
}

linux_job() {
  step "Build or fetch the native runtime, tests and Web CLI"
  nix --extra-experimental-features 'nix-command flakes' build --no-link .#ci-core .#cli
  nix --extra-experimental-features 'nix-command flakes' build .#ci-core --out-link build-ci
  local web system
  web=$(nix --extra-experimental-features 'nix-command flakes' build '.#web^out' --no-link --print-out-paths)
  export TSUGUMI_WEB_DIST="$web/share/tsugumi/web/dist"
  step "Public core, Agent and CLI RPC tests"
  core_checks
  step "Web UI typecheck and behavior contracts"
  system=$(nix --extra-experimental-features 'nix-command flakes' eval --raw --impure --expr builtins.currentSystem)
  nix --extra-experimental-features 'nix-command flakes' build --no-link ".#checks.$system.web"
  step "Documentation site player is up to date"
  nix --extra-experimental-features 'nix-command flakes' build --no-link ".#checks.$system.site-player"
  step "Core integration tests with the real host environment"
  core_run python3 -m pytest -q tests/core/integration
  step "Python tests"
  core_run python3 -m pytest -q tests/python
  if [ "$system" = aarch64-linux ]; then
    step "Cache the VM runtime"
    nix --extra-experimental-features 'nix-command flakes' build --no-link .#vm-system
  fi
  step "Prompt probe scoring (offline)"
  python3 -m unittest discover -s tests/prompt
}

linux_in_container() {
  local image=nixos/nix:latest
  step "Linux job in a container ($image)"
  # The tracked files as they are now (a file not yet added is left out, as a
  # commit would leave it out); macOS tar leaves out its ._ resource files.
  git ls-files -z -c |
    python3 -c 'import os, sys; sys.stdout.buffer.write(b"".join(p + b"\0" for p in sys.stdin.buffer.read().split(b"\0") if p and os.path.lexists(p)))' |
    COPYFILE_DISABLE=1 tar --null -T - -cf - |
    docker run --rm -i --privileged -v tsugumi-ci-nix:/nix "$image" bash -c '
      set -e
      mkdir -p /src && tar -xf - -C /src && cd /src
      git config --global --add safe.directory /src
      git init -q && git add -A
      echo "experimental-features = nix-command flakes" >> /etc/nix/nix.conf
      bash tests/ci-local.sh linux'
}

mac_job() {
  step "Build or fetch the native runtime and public RPC tests"
  nix --extra-experimental-features 'nix-command flakes' build .#ci-core --out-link build-ci
  step "Core RPC behavior tests"
  core_checks
  step "Card isolation on a Mac host (Seatbelt)"
  core_run python3 -m pytest -q tests/core/integration/test_host_isolation.py tests/core/integration/test_host_sandbox.py
  step "Model, RPC, Agent and launcher (macOS/CoreTests)"
  core_run /usr/bin/swift test --package-path macOS/CoreTests
  step "Build the Dawn renderer (the app package links it)"
  ui/renderer/build.sh
  step "App logic (macOS/Tests)"
  core_run /usr/bin/swift test --package-path macOS
}

case "${1:-all}" in
  mac) mac_job ;;
  linux) if [ "$(uname)" = Linux ]; then linux_job; else linux_in_container; fi ;;
  all)
    if [ "$(uname)" = Darwin ]; then mac_job; linux_in_container; else linux_job; fi ;;
  *) echo "usage: $0 [all|mac|linux]" >&2; exit 2 ;;
esac
step "All checks passed"
