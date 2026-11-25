#!/usr/bin/env bash
# Builds dist/tsugumi-linux.tar.gz and its checksum, what cli/install.sh installs
# on Linux: the committed source the flake's `cli` package builds from (the same
# for x86_64 and aarch64; everything it fetches is pinned by flake.lock and
# ui/web/package-lock.json).
#
#   cli/package-linux.sh
set -euo pipefail
# Keep macOS extended attributes out of the portable source archive.
export COPYFILE_DISABLE=1
repo="$(cd "$(dirname "$0")/.." && pwd)"
dist="$repo/dist"
mkdir -p "$dist"
stage=$(mktemp -d "$dist/cli-linux.XXXXXX")
trap 'rm -rf "$stage"' EXIT
# What nix/cli.nix and nix/core.nix read, and the flake itself.
git -C "$repo" archive --format=tar --prefix=tsugumi/ HEAD \
  flake.nix flake.lock VERSION CMakeLists.txt cmake util rpc tsugumid \
  hostd/CMakeLists.txt hostd/cmake hostd/src hostd/cache hostd/private hostd/agent/src hostd/agent/private hostd/agent/Shared \
  client/CMakeLists.txt client/cmake client/include client/src cli/CMakeLists.txt cli/src workerd \
  protocol/CMakeLists.txt protocol/include protocol/trace_codec.cc 'protocol/*.fbs' \
  nix/core.nix nix/core-source.nix nix/cli.nix nix/demos.nix nix/web.nix nix/client-wasm.nix nix/card-entry.nix nix/worker.nix \
  third-party \
  vm/guest/CMakeLists.txt vm/guest/src vm/guest/vm-system.nix \
  ui/web/package.json ui/web/package-lock.json 'ui/web/tsconfig*.json' \
  ui/web/scripts/build.mjs ui/web/scripts/install-viewer.mjs ui/web/scripts/build-client.sh ui/web/scripts/gen.sh ui/web/scripts/check-boundaries.mjs \
  ui/web/src ui/web/shared ui/web/third-party ui/shared/modules \
  ui/shared/i18n/en.json ui/shared/dawn \
  docs/CardGuide.md docs/site/player/build.mjs docs/site/player/narration.json \
  docs/site/player-data/assets docs/assets/tsugumi-icon.svg | tar -xf - -C "$stage"
rm "$stage/tsugumi/hostd/agent/Shared/generate.py" "$stage/tsugumi/hostd/agent/Shared/mcp-transport.md"
find "$stage" -type f -name README.md -delete
# The source distribution builds the CLI only; repository checks, documentation
# tools and development shells are not part of its public flake.
# Retain the exact same input declarations as the development flake.
sed '/^  outputs =/,$d' "$repo/flake.nix" > "$stage/tsugumi/flake.nix"
cat >> "$stage/tsugumi/flake.nix" <<'NIX'
  outputs =
    inputs:
    inputs.flake-parts.lib.mkFlake { inherit inputs; } {
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      perSystem =
        { pkgs, ... }:
        let
          tsugumid = pkgs.callPackage ./nix/core.nix {
            inherit (inputs) hosq webgpu-headers;
            cardNixpkgs = {
              rev = inputs.nixpkgs.rev;
              narHash = inputs.nixpkgs.narHash;
            };
          };
          client-wasm = pkgs.callPackage ./nix/client-wasm.nix { };
          demos = pkgs.callPackage ./nix/demos.nix { inherit (inputs) tsugumi-demos; };
          web = pkgs.callPackage ./nix/web.nix {
            inherit client-wasm demos;
            inherit (inputs) webgpu-headers;
          };
          cli = pkgs.callPackage ./nix/cli.nix { inherit tsugumid web; };
        in
        {
          packages = {
            inherit cli;
            default = cli;
          };
        };
    };
}
NIX
tar -czf "$dist/tsugumi-linux.tar.gz" -C "$stage" tsugumi
(cd "$dist" && shasum -a 256 tsugumi-linux.tar.gz > tsugumi-linux.tar.gz.sha256)
ls -l "$dist/tsugumi-linux.tar.gz"
