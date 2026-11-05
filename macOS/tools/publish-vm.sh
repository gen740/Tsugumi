#!/usr/bin/env bash
# Publishes the boot disk of dist/tsugumi-vm (vm-boot.nix: kernel and a small
# root file system with Nix, nothing of Tsugumi) as a release of
# gen740/tsugumi-vm (public), tagged with the hash of its store name, so
# `build-image.sh --vm` downloads it instead of building the kernel. Its licenses
# (vm-inventory.py; the app's build takes them from here) and its corresponding
# source (vm-source-bundle.py) go with it, also to a release without them yet.
# Keep a release's source for as long as a release of the app carries that disk.
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
vm="$repo/dist/tsugumi-vm"
releases="${TSUGUMI_VM_RELEASES:-gen740/tsugumi-vm}"
name="$(cat "$vm/vm-version")"
tag="${name%%-*}"
assets="$(gh release view "$tag" -R "$releases" --json assets --jq '.assets[].name' 2>/dev/null || true)"
if grep -qx licenses.tar.gz <<<"$assets" && grep -qx sources.tar.gz <<<"$assets"; then
  echo "$releases already has $name"
  exit 0
fi
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
"$repo/macOS/tools/vm-inventory.py" "$tmp/vm-boot"
tar -czf "$tmp/licenses.tar.gz" -C "$tmp" vm-boot
"$repo/macOS/tools/vm-source-bundle.py" "$tmp/vm-boot" "$tmp/sources.tar.gz"
(cd "$tmp" && shasum -a 256 sources.tar.gz > sources.tar.gz.sha256)
if [ -n "$assets" ]; then
  gh release upload "$tag" -R "$releases" --clobber "$tmp/licenses.tar.gz" "$tmp/sources.tar.gz" "$tmp/sources.tar.gz.sha256"
  exit 0
fi
cp "$vm/vmlinux" "$vm/rootfs.erofs" "$vm/vm-version" "$tmp/"
(cd "$tmp" && shasum -a 256 vmlinux rootfs.erofs vm-version > SHA256SUMS)
gh release create "$tag" -R "$releases" --title "$name" --notes "Boot disk of the shared VM: $name (kernel and a root file system with Nix, nixpkgs only). Its corresponding source is sources.tar.gz, its license texts licenses.tar.gz." \
  "$tmp/vmlinux" "$tmp/rootfs.erofs" "$tmp/vm-version" "$tmp/SHA256SUMS" "$tmp/licenses.tar.gz" \
  "$tmp/sources.tar.gz" "$tmp/sources.tar.gz.sha256"
