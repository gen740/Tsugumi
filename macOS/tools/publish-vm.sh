#!/usr/bin/env bash
# Publishes the base of dist/tsugumi-vm (vm-base.nix: kernel and root file
# system, nixpkgs only, nothing of Tsugumi) as a release of gen740/tsugumi-vm
# (public), tagged with the hash of its store name, so `build-image.sh --vm`
# downloads it and builds only Tsugumi's layer.
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
vm="$repo/dist/tsugumi-vm"
releases="${TSUGUMI_VM_RELEASES:-gen740/tsugumi-vm}"
name="$(cat "$vm/base-version")"
tag="${name%%-*}"
if gh release view "$tag" -R "$releases" >/dev/null 2>&1; then
  echo "$releases already has $name"
  exit 0
fi
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
cp "$vm/vmlinux" "$vm/rootfs.erofs" "$vm/base-version" "$tmp/"
(cd "$tmp" && shasum -a 256 vmlinux rootfs.erofs base-version > SHA256SUMS)
gh release create "$tag" -R "$releases" --title "$name" --notes "Base of the shared VM: $name (nixpkgs only)." \
  "$tmp/vmlinux" "$tmp/rootfs.erofs" "$tmp/base-version" "$tmp/SHA256SUMS"
