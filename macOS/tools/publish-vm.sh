#!/usr/bin/env bash
# Publishes the boot disk of dist/tsugumi-vm (vm-boot.nix: kernel and a small
# root file system with Nix, nothing of Tsugumi) as a release of
# gen740/tsugumi-vm (public), tagged with the hash of its store name, so
# `build-image.sh --vm` downloads it instead of building the kernel.
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
vm="$repo/dist/tsugumi-vm"
releases="${TSUGUMI_VM_RELEASES:-gen740/tsugumi-vm}"
name="$(cat "$vm/vm-version")"
tag="${name%%-*}"
if gh release view "$tag" -R "$releases" >/dev/null 2>&1; then
  echo "$releases already has $name"
  exit 0
fi
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
cp "$vm/vmlinux" "$vm/rootfs.erofs" "$vm/vm-version" "$tmp/"
(cd "$tmp" && shasum -a 256 vmlinux rootfs.erofs vm-version > SHA256SUMS)
gh release create "$tag" -R "$releases" --title "$name" --notes "Boot disk of the shared VM: $name (kernel and a root file system with Nix, nixpkgs only)." \
  "$tmp/vmlinux" "$tmp/rootfs.erofs" "$tmp/vm-version" "$tmp/SHA256SUMS"
