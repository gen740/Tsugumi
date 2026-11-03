#!/usr/bin/env bash
# Builds the shared VM's Linux parts inside an Apple container (no Linux host
# or remote builder needed).
#
#   macOS/Container/build-image.sh --vm       # the shared VM's boot disk into dist/tsugumi-vm/
#                                             # (vm-boot.nix: kernel and a small rootfs with Nix),
#                                             # downloaded from gen740/tsugumi-vm when released;
#                                             # Tsugumi itself is built inside the VM (vm-system.nix)
#   macOS/Container/build-image.sh --check    # only build the Linux tsugumid package
#   macOS/Container/build-image.sh --kernel   # build the guest kernel into dist/tsugumi-kernel
#
# Pinned builder: docker.io/nixos/nix:2.31.2 (see versions.env). The builder's
# Nix store lives in the volume tsugumi-builder-nix and is reused.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
source "$here/versions.env"
case "${1:-}" in
  --vm) attr=vm ;;
  --check) attr=tsugumid ;;
  --kernel) attr=kernel ;;
  *) sed -n '2,10s/^# \{0,1\}//p' "$0" >&2; exit 2 ;;
esac
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
# Puts vmlinux, rootfs.erofs and vm-version of a folder in place of
# dist/tsugumi-vm, whole: a failure keeps the last one.
install_vm() {
  local new="$dist_vm.new"
  rm -rf "$new"
  mkdir -p "$new"
  cp "$1/vmlinux" "$1/rootfs.erofs" "$1/vm-version" "$new/"
  rm -rf "$dist_vm"
  mv "$new" "$dist_vm"
  echo "vm: $dist_vm ($(cat "$dist_vm/vm-version"), rootfs $(stat -f %z "$dist_vm/rootfs.erofs") bytes)"
}
if [ "$attr" = vm ]; then
  # The boot disk changes only with nixpkgs and vm-boot.nix: kept from the
  # last build, or downloaded from gen740/tsugumi-vm where it is released
  # under the hash of its store name (macOS/tools/publish-vm.sh), evaluated
  # here without building anything. TSUGUMI_VM_BUILD=1 builds it.
  boot=$(nix --extra-experimental-features "nix-command flakes" eval --raw \
    "git+file://$stage#packages.aarch64-linux.vm-boot.outPath" 2>/dev/null || true)
  boot=${boot#/nix/store/}
  if [ "${TSUGUMI_VM_BUILD:-}" != 1 ] && [ -n "$boot" ]; then
    if [ "$(cat "$dist_vm/vm-version" 2>/dev/null)" = "$boot" ]; then
      echo "vm: $dist_vm is $boot"
      exit 0
    fi
    url="https://github.com/${TSUGUMI_VM_RELEASES:-gen740/tsugumi-vm}/releases/download/${boot%%-*}"
    if curl -fsSL -o "$out/SHA256SUMS" "$url/SHA256SUMS" 2>/dev/null; then
      echo "downloading the boot disk $boot"
      for f in vmlinux rootfs.erofs vm-version; do curl -fL --retry 3 -o "$out/$f" "$url/$f"; done
      (cd "$out" && shasum -a 256 -c SHA256SUMS >/dev/null)
      [ "$(cat "$out/vm-version")" = "$boot" ]
      install_vm "$out"
      rm -rf "$out"
      exit 0
    fi
    echo "no release of the boot disk $boot; building it"
  fi
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
      if [ "'"$attr"'" = vm ]; then
        # Links into the store: copy from the chroot store under /cache.
        for f in vmlinux rootfs.erofs; do cp "/cache$(readlink /cache$img/$f)" /out/$f; done
        cp /cache$img/vm-version /out/
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
if [ "$attr" = vm ]; then
  install_vm "$out"
  rm -rf "$out"
  exit 0
fi
rm -rf "$out"
