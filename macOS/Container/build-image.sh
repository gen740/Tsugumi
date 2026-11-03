#!/usr/bin/env bash
# Builds the tsugumi-env image inside an Apple container (no Linux host or
# remote builder needed) and loads it into the local image store.
#
#   macOS/Container/build-image.sh            # build and load
#   macOS/Container/build-image.sh --check    # only build the Linux tsugumid package
#   macOS/Container/build-image.sh --kernel   # build the guest kernel into dist/tsugumi-kernel
#   macOS/Container/build-image.sh --vm       # the shared VM into dist/tsugumi-vm/: Tsugumi's layer
#                                             # (vm-layer.nix) on the base (vm-base.nix), which is
#                                             # downloaded from gen740/tsugumi-vm when released
#
# Pinned builder: docker.io/nixos/nix:2.31.2 (see versions.env). The builder's
# Nix store lives in the volume tsugumi-builder-nix and is reused.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
source "$here/versions.env"
attr=image
[ "${1:-}" = "--check" ] && attr=tsugumid
[ "${1:-}" = "--kernel" ] && attr=kernel
[ "${1:-}" = "--vm" ] && attr=vm
# The builder VM runs on Tsugumi's guest kernel when it exists (the stock
# kernel's SELinux oops leaves builder VMs stuck on shutdown; kernel.nix).
kernel_args=()
kernel="${TSUGUMI_KERNEL:-$repo/dist/tsugumi-kernel}"
[ -f "$kernel" ] && kernel_args=(--kernel "$kernel")

stage="$(mktemp -d "${TMPDIR:-/tmp}/tsugumi-src.XXXXXX")"
out="$(mktemp -d "${TMPDIR:-/tmp}/tsugumi-image.XXXXXX")"
trap 'rm -rf "$stage"' EXIT
# Exactly the files Git would ship (tracked + untracked, not ignored).
(cd "$repo" && git ls-files -z -co --exclude-standard | xargs -0 -I{} rsync -R "{}" "$stage/")
(cd "$stage" && git init -q && git add -A && git -c user.email=b@t -c user.name=b commit -qm build)

dist_vm="$repo/dist/tsugumi-vm"
if [ "$attr" = vm ]; then
  # The base (nixpkgs only, vm-base.nix) is kept from the last build, or
  # downloaded from gen740/tsugumi-vm where it is released under the hash of
  # its store name, evaluated here without building anything
  # (macOS/tools/publish-vm.sh); only Tsugumi's layer is built then.
  # TSUGUMI_VM_BUILD=1 builds the base too.
  base=$(nix --extra-experimental-features "nix-command flakes" eval --raw \
    "git+file://$stage#packages.aarch64-linux.vm-base.outPath" 2>/dev/null || true)
  base=${base#/nix/store/}
  mkdir -p "$out/base"
  if [ "${TSUGUMI_VM_BUILD:-}" = 1 ] || [ -z "$base" ]; then
    :
  elif [ "$(cat "$dist_vm/base-version" 2>/dev/null)" = "$base" ]; then
    cp "$dist_vm/vmlinux" "$dist_vm/rootfs.erofs" "$dist_vm/base-version" "$out/base/"
  else
    url="https://github.com/${TSUGUMI_VM_RELEASES:-gen740/tsugumi-vm}/releases/download/${base%%-*}"
    if curl -fsSL -o "$out/base/SHA256SUMS" "$url/SHA256SUMS" 2>/dev/null; then
      echo "downloading the base $base"
      for f in vmlinux rootfs.erofs base-version; do curl -fL --retry 3 -o "$out/base/$f" "$url/$f"; done
      (cd "$out/base" && shasum -a 256 -c SHA256SUMS >/dev/null)
      [ "$(cat "$out/base/base-version")" = "$base" ]
    else
      echo "no release of the base $base; building it"
    fi
  fi
  [ -f "$out/base/rootfs.erofs" ] && attr=vm-layer
fi

volume="${TSUGUMI_BUILDER_VOLUME:-tsugumi-builder-nix}"
container volume inspect "$volume" >/dev/null 2>&1 || container volume create -s 40G "$volume" >/dev/null
# The builder runs detached and signals completion through /out, so the build
# never depends on how quickly the VM is torn down afterwards.
builder="tsugumi-image-builder-$(date +%s)"
container run -d --rm --name "$builder" ${kernel_args[@]+"${kernel_args[@]}"} -c "${TSUGUMI_BUILDER_CPUS:-8}" -m "${TSUGUMI_BUILDER_MEMORY:-16G}" \
  -v "$volume:/cache" -v "$stage:/src:ro" -v "$out:/out" \
  "$TSUGUMI_BUILDER_IMAGE" sh -uc '
    # Any failing step ends the shell (set -e, not inside an && list where it
    # would be ignored), and the trap then marks the build failed.
    trap "[ -e /out/done ] || touch /out/failed" EXIT
    set -e
    exec > /out/log 2>&1
    {
      # Chroot store under /cache: persistent across builds, separate from the image store.
      nixf() { nix --extra-experimental-features "nix-command flakes" --store /cache "$@"; }
      # build.log grows while it builds (watch it from the Mac).
      img=$(nixf build --no-link --print-out-paths -L "git+file:///src#packages.aarch64-linux.'"$attr"'" 2>&1 | grep -v "^copying path" | tee /out/build.log | tail -1)
      # A failed build prints no store path: stop here instead of copying nothing.
      case "$img" in /nix/store/*) [ -e "/cache$img" ] ;; *) false ;; esac
      if [ "'"$attr"'" = kernel ]; then cp /cache$img/vmlinux /out/tsugumi-kernel; fi
      if [ "'"$attr"'" = vm-layer ]; then cp /cache$img/tsugumi.erofs /out/; basename "$img" > /out/layer-version; fi
      if [ "'"$attr"'" = vm ]; then
        # Links into the store: copy from the chroot store under /cache.
        for f in vmlinux rootfs.erofs tsugumi.erofs; do cp "/cache$(readlink /cache$img/$f)" /out/$f; done
        cp /cache$img/layer-version /out/; mkdir -p /out/base; cp /cache$img/base-version /out/base/
        mv /out/vmlinux /out/rootfs.erofs /out/base/
      fi
      if [ "'"$attr"'" = image ]; then
        cp /cache$img /out/tsugumi-env.tar.gz
        # OCI layout for `container image load`, converted by skopeo run in the store namespace.
        nixf shell nixpkgs#skopeo -c skopeo --insecure-policy copy \
          "docker-archive:/out/tsugumi-env.tar.gz" "oci-archive:/out/tsugumi-env.tar:tsugumi-env:'"$TSUGUMI_IMAGE_TAG"'"
      fi
    }
    touch /out/done
  ' >/dev/null
echo "building $attr in $builder (progress: $out/build.log)"
while [ ! -e "$out/done" ] && [ ! -e "$out/failed" ]; do sleep 2; done
cat "$out/build.log" 2>/dev/null | tail -5
if [ -e "$out/failed" ]; then tail -40 "$out/log"; exit 1; fi
if [ "$attr" = kernel ]; then
  mkdir -p "$repo/dist"
  cp "$out/tsugumi-kernel" "$repo/dist/tsugumi-kernel"
  echo "kernel: $repo/dist/tsugumi-kernel ($(stat -f %z "$repo/dist/tsugumi-kernel") bytes)"
  rm -rf "$out"
  exit 0
fi
if [ "$attr" = vm ] || [ "$attr" = vm-layer ]; then
  # Assembled next to it and swapped in whole: a failure keeps the last VM.
  new="$dist_vm.new"
  rm -rf "$new"
  mkdir -p "$new"
  cp "$out/base/vmlinux" "$out/base/rootfs.erofs" "$out/base/base-version" "$out/tsugumi.erofs" "$out/layer-version" "$new/"
  echo "$(cat "$new/base-version") $(cat "$new/layer-version")" > "$new/vm-version"
  rm -rf "$dist_vm"
  mv "$new" "$dist_vm"
  echo "vm: $dist_vm (base $(cat "$dist_vm/base-version"), layer $(cat "$dist_vm/layer-version"), $(stat -f %z "$dist_vm/tsugumi.erofs") bytes)"
  rm -rf "$out"
  exit 0
fi
[ "$attr" = image ] || exit 0
# `image load` keeps an existing tag; replace it explicitly. Running containers keep their image.
container image rm "$TSUGUMI_IMAGE:$TSUGUMI_IMAGE_TAG" >/dev/null 2>&1 || true
container image load --input "$out/tsugumi-env.tar"
# Keep the OCI archive when requested (the app bundle ships it for first start).
if [ -n "${TSUGUMI_IMAGE_OUT:-}" ]; then
  mkdir -p "$(dirname "$TSUGUMI_IMAGE_OUT")"
  cp "$out/tsugumi-env.tar" "$TSUGUMI_IMAGE_OUT"
fi
echo "loaded tsugumi-env:$TSUGUMI_IMAGE_TAG; compressed image: $(stat -f %z "$out/tsugumi-env.tar.gz") bytes"
rm -rf "$out"
