#!/usr/bin/env bash
# Runs the checks of .github/workflows/ci.yml on this machine. GitHub Actions
# is turned off for the repository, so a change is merged once this passes.
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
nixdev() { nix --extra-experimental-features 'nix-command flakes' develop -c bash -c "$1"; }
step() { printf '\n== %s\n' "$1"; }

linux_job() {
  # Card shell steps get the dev shell's bash and tools, as in CI.
  step "Build tsugumid and run the C++ tests"
  nixdev 'set -e
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DTSUGUMI_BWRAP="$(command -v bwrap)" \
      -DTSUGUMI_BASH="$(command -v bash)" -DTSUGUMI_CARD_PATH="$PATH"
    ninja -C build
    ctest --test-dir build --output-on-failure'
  step "Web UI typecheck, behavior contracts and build"
  nixdev 'cd web && npm ci && npm run gen && npm run typecheck && npm test && npm run build'
  step "The tsugumi command builds (nix/cli.nix)"
  nix --extra-experimental-features 'nix-command flakes' build .#cli --no-link
  step "Documentation site player is up to date"
  nixdev 'node docs/site/player/build.mjs --check'
  step "Python tests"
  nixdev 'cd tests/python && python3 -m pytest -q'
}

linux_in_container() {
  local image=nixos/nix:latest
  step "Linux job in a container ($image)"
  # The tracked files as they are now (a file not yet added is left out, as a
  # commit would leave it out); macOS tar leaves out its ._ resource files.
  git ls-files -z -c | COPYFILE_DISABLE=1 tar --null -T - -cf - |
    docker run --rm -i --privileged -v tsugumi-ci-nix:/nix "$image" bash -c '
      set -e
      mkdir -p /src && tar -xf - -C /src && cd /src
      git config --global --add safe.directory /src
      git init -q && git add -A
      echo "experimental-features = nix-command flakes" >> /etc/nix/nix.conf
      bash tests/ci-local.sh linux'
}

mac_job() {
  # The worker runs the Cards an Agent test builds (TeamTests runs bash Cards).
  step "Build the host tsugumid and its worker"
  nixdev 'cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && ninja -C build tsugumid tsugumi-worker'
  step "Card isolation on a Mac host (Seatbelt)"
  nixdev 'cd tests/python && python3 -m pytest -q test_host_isolation.py test_host_sandbox.py'
  TSUGUMI_FLATC="$(nixdev 'command -v flatc')"
  export TSUGUMI_FLATC
  step "Model, RPC, Agent and launcher (macOS/CoreTests)"
  (cd macOS/CoreTests && swift test)
  step "Build the Dawn renderer (the app package links it)"
  [ -f build-renderer/ui/renderer/libtsugumi_renderer.dylib ] || ui/renderer/build.sh
  step "App logic (macOS/Tests)"
  (cd macOS && swift test)
}

case "${1:-all}" in
  mac) mac_job ;;
  linux) if [ "$(uname)" = Linux ]; then linux_job; else linux_in_container; fi ;;
  all)
    if [ "$(uname)" = Darwin ]; then mac_job; linux_in_container; else linux_job; fi ;;
  *) echo "usage: $0 [all|mac|linux]" >&2; exit 2 ;;
esac
step "All checks passed"
