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
  client/CMakeLists.txt client/cmake client/include client/src cli/CMakeLists.txt cli/include cli/src sdk workerd \
  protocol/CMakeLists.txt protocol/include protocol/trace_codec.cc 'protocol/*.fbs' \
  nix/core.nix nix/core-source.nix nix/cli.nix nix/web.nix nix/client-wasm.nix \
  third-party \
  vm/guest/CMakeLists.txt vm/guest/src vm/guest/vm-system.nix \
  ui/web/package.json ui/web/package-lock.json 'ui/web/tsconfig*.json' \
  ui/web/scripts/build.mjs ui/web/scripts/build-client.sh ui/web/scripts/gen.sh ui/web/scripts/check-boundaries.mjs \
  ui/web/src ui/web/shared ui/web/third-party ui/shared/modules 'ui/shared/templates/*.json' \
  ui/shared/i18n/en.json \
  demos docs/CardGuide.md docs/site/player/build.mjs docs/site/player/narration.json \
  docs/site/player-data/assets docs/assets/tsugumi-icon.svg | tar -xf - -C "$stage"
# Repository-only generators and their template inputs are already represented
# by the generated host catalog. Lock helpers remain part of the authoring SDK.
rm -rf "$stage/tsugumi/sdk/go/template" "$stage/tsugumi/sdk/rust/template"
rm "$stage/tsugumi/sdk/codegen/scaffold.py" \
  "$stage/tsugumi/hostd/agent/Shared/generate.py" "$stage/tsugumi/hostd/agent/Shared/mcp-transport.md"
find "$stage" -type f -name README.md -delete
# The source distribution builds the CLI only; repository checks, documentation
# tools and development shells are not part of its public flake.
cat > "$stage/tsugumi/flake.nix" <<'NIX'
{
  nixConfig = {
    extra-substituters = [ "https://tsugumi.cachix.org" ];
    extra-trusted-public-keys = [ "tsugumi.cachix.org-1:am26Q4VFWYLyJ+t+O5j03MmMVmZ2Ifw6cbNUoLhlrhI=" ];
  };
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  inputs.hosq = {
    url = "github:CLI20-dev/hosq/1eb894224b0feffbb5b81d075d3e2c76160f99a1";
    flake = false;
  };
  inputs.flake-parts = {
    url = "github:hercules-ci/flake-parts";
    inputs.nixpkgs-lib.follows = "nixpkgs";
  };
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
            hosq = inputs.hosq;
            cardNixpkgs = {
              rev = inputs.nixpkgs.rev;
              narHash = inputs.nixpkgs.narHash;
            };
          };
          client-wasm = pkgs.callPackage ./nix/client-wasm.nix { };
          web = pkgs.callPackage ./nix/web.nix { inherit client-wasm; };
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
