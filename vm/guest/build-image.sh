#!/usr/bin/env bash
# Resolve the current guest definition: verified local artifact, matching Actions
# artifact (gh auth login), then a local Nix/Apple-container build.
# TSUGUMI_VM_BUILD=1 bypasses caches. TSUGUMI_VM_ARTIFACT_ONLY=1 forbids fallback
# and requires the artifact's revision to match HEAD (release packaging).
# --check builds tsugumid; --kernel builds the guest kernel; --vm resolves the guest.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
source "$here/versions.env"
case "${1:-}" in
--vm) attr=vm ;;
--check) attr=tsugumid ;;
--kernel) attr=kernel ;;
*)
  echo "usage: $0 --vm|--check|--kernel" >&2
  exit 2
  ;;
esac
stage="$(mktemp -d "${TMPDIR:-/tmp}/tsugumi-src.XXXXXX")"
out="$(mktemp -d "${TMPDIR:-/tmp}/tsugumi-image.XXXXXX")"
trap 'rm -rf "$stage" "$out"' EXIT
# Snapshot current tracked and untracked intended inputs, including local edits.
python3 "$repo/vm/guest/tools/vm-snapshot.py" "$repo" "$stage"
revision="$(git -C "$repo" rev-parse HEAD)"
if [ -n "$(git -C "$repo" status --porcelain)" ]; then revision="$revision-dirty"; fi
dist_vm="$repo/dist/tsugumi-vm"
verify=(python3 "$stage/vm/guest/tools/vm-artifact.py" verify)
if [ "$attr" = vm ]; then
  identity="$(nix eval --raw "path:$stage#packages.aarch64-linux.vm-boot.outPath")"
  identity="${identity##*/}"
  if [ "${TSUGUMI_VM_ARTIFACT_ONLY:-}" = 1 ]; then
    "${verify[@]}" "$dist_vm" "$identity" --revision "$revision"
    exit 0
  fi
  if [ "${TSUGUMI_VM_BUILD:-}" != 1 ]; then
    if "${verify[@]}" "$dist_vm" "$identity" 2>/dev/null; then
      echo "vm: reusing $identity"
      exit 0
    fi
    if command -v gh >/dev/null && python3 "$stage/vm/guest/tools/vm-download.py" "$identity" "$out" "${GITHUB_REPOSITORY:-gen740/Tsugumi}" &&
      "${verify[@]}" "$out" "$identity"; then
      mkdir -p "$repo/dist"
      rm -rf "$dist_vm"
      mv "$out" "$dist_vm"
      exit 0
    fi
    echo "no successful CI artifact for $identity; building locally"
  fi
fi
if [ "$(uname -s)" = Linux ] || [ -n "${TSUGUMI_VM_NIX_BUILDERS:-}" ]; then
  build_args=()
  if [ -n "${TSUGUMI_VM_NIX_BUILDERS:-}" ]; then
    build_args=(--builders "$TSUGUMI_VM_NIX_BUILDERS")
    export NIX_CONFIG="${NIX_CONFIG:-}
builders = $TSUGUMI_VM_NIX_BUILDERS"
  fi
  image="$(nix build --no-link --print-out-paths -L ${build_args[@]+"${build_args[@]}"} "path:$stage#packages.aarch64-linux.$attr")"
  if [ "$attr" = vm ]; then
    nix shell --inputs-from "$stage" nixpkgs#nix nixpkgs#python3 nixpkgs#erofs-utils nixpkgs#libarchive -c \
      python3 "$stage/vm/guest/tools/vm-artifact.py" build "$image" "$out" --revision "$revision"
  elif [ "$attr" = kernel ]; then cp "$image/vmlinux" "$out/tsugumi-kernel"; fi
else
  kernel_args=()
  [ ! -f "$repo/dist/tsugumi-kernel" ] || kernel_args=(--kernel "$repo/dist/tsugumi-kernel")
  volume="${TSUGUMI_BUILDER_VOLUME:-tsugumi-builder-nix}"
  container volume inspect "$volume" >/dev/null 2>&1 || container volume create -s 40G "$volume" >/dev/null
  builder="tsugumi-image-builder-$(date +%s)"
  # Keep the builder image's bootstrap /nix visible. The persistent build store
  # remains rooted under /cache, as in the existing Apple-container builder.
  container run -d --rm --name "$builder" ${kernel_args[@]+"${kernel_args[@]}"} -c "${TSUGUMI_BUILDER_CPUS:-8}" -m "${TSUGUMI_BUILDER_MEMORY:-16G}" \
    -v "$volume:/cache" -v "$stage:/src:ro" -v "$out:/out" \
    -e "GUEST_ATTRIBUTE=$attr" -e "GUEST_REVISION=$revision" \
    "$TSUGUMI_BUILDER_IMAGE" sh -uc '
      trap "[ -e /out/done ] || touch /out/failed" EXIT
      set -e
      exec > /out/log 2>&1
      # Use the flake-pinned Nix for both derivation JSON and source export.
      # The builder image enables no experimental features.
      export NIX_CONFIG="experimental-features = nix-command flakes
fsync-store-paths = true
fsync-metadata = true"
      nix shell --inputs-from /src nixpkgs#nix nixpkgs#python3 nixpkgs#erofs-utils nixpkgs#libarchive -c sh /src/vm/guest/build-in-container.sh
      sync -f /cache
      touch /out/done
    ' >/dev/null
  echo "building $attr in $builder (progress: $out/log)"
  while [ ! -e "$out/done" ] && [ ! -e "$out/failed" ]; do sleep 2; done
  tail -10 "$out/log"
  [ ! -e "$out/failed" ] || exit 1
  rm -f "$out/done" "$out/log"
fi
mkdir -p "$repo/dist"
if [ "$attr" = vm ]; then
  "${verify[@]}" "$out" "$identity"
  rm -rf "$dist_vm"
  mv "$out" "$dist_vm"
elif [ "$attr" = kernel ]; then
  cp "$out/tsugumi-kernel" "$repo/dist/tsugumi-kernel"
fi
