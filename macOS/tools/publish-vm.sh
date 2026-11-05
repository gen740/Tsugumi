#!/usr/bin/env bash
# Publishes the boot disk of dist/tsugumi-vm (vm-boot.nix: kernel and a small
# root file system with Nix, nothing of Tsugumi) as a release of
# gen740/tsugumi-vm (public), tagged with the hash of its store name, so
# `build-image.sh --vm` downloads it instead of building the kernel. Its licenses
# (third-party/vm-boot, from macOS/tools/vm-inventory.py) and its corresponding
# source (vm-source-bundle.py) go with it, also to a release that exists already.
# Keep a release's source for as long as a release of the app carries that disk.
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
vm="$repo/dist/tsugumi-vm"
releases="${TSUGUMI_VM_RELEASES:-gen740/tsugumi-vm}"
name="$(cat "$vm/vm-version")"
tag="${name%%-*}"
listed="$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["vm_version"])' "$repo/third-party/vm-boot/inventory.json")"
[ "$listed" = "$name" ] || { echo "third-party/vm-boot describes $listed, not $name: run macOS/tools/vm-inventory.py" >&2; exit 1; }
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
tar -czf "$tmp/licenses.tar.gz" -C "$repo/third-party" vm-boot
if gh release view "$tag" -R "$releases" >/dev/null 2>&1; then
  gh release upload "$tag" -R "$releases" --clobber "$tmp/licenses.tar.gz"
  # The source of a disk does not change: uploaded once.
  if ! gh release view "$tag" -R "$releases" --json assets --jq '.assets[].name' | grep -qx sources.tar.gz; then
    "$repo/macOS/tools/vm-source-bundle.py" "$tmp/sources.tar.gz"
    (cd "$tmp" && shasum -a 256 sources.tar.gz > sources.tar.gz.sha256)
    gh release upload "$tag" -R "$releases" "$tmp/sources.tar.gz" "$tmp/sources.tar.gz.sha256"
  fi
  echo "$releases already has $name (its licenses and source are in place)"
  exit 0
fi
"$repo/macOS/tools/vm-source-bundle.py" "$tmp/sources.tar.gz"
(cd "$tmp" && shasum -a 256 sources.tar.gz > sources.tar.gz.sha256)
cp "$vm/vmlinux" "$vm/rootfs.erofs" "$vm/vm-version" "$tmp/"
(cd "$tmp" && shasum -a 256 vmlinux rootfs.erofs vm-version > SHA256SUMS)
gh release create "$tag" -R "$releases" --title "$name" --notes "Boot disk of the shared VM: $name (kernel and a root file system with Nix, nixpkgs only). Its corresponding source is sources.tar.gz, its license texts licenses.tar.gz." \
  "$tmp/vmlinux" "$tmp/rootfs.erofs" "$tmp/vm-version" "$tmp/SHA256SUMS" "$tmp/licenses.tar.gz" \
  "$tmp/sources.tar.gz" "$tmp/sources.tar.gz.sha256"
