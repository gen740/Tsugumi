#!/usr/bin/env bash
# Builds the tsugumi-env image inside an Apple container (no Linux host or
# remote builder needed) and loads it into the local image store.
#
#   macOS/Container/build-image.sh            # build and load
#   macOS/Container/build-image.sh --check    # only build the Linux tsugumid package
#
# Pinned builder: docker.io/nixos/nix:2.31.2 (see versions.env). The builder's
# Nix store lives in the volume tsugumi-builder-nix and is reused.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
source "$here/versions.env"
attr=image
[ "${1:-}" = "--check" ] && attr=tsugumid

stage="$(mktemp -d "${TMPDIR:-/tmp}/tsugumi-src.XXXXXX")"
out="$(mktemp -d "${TMPDIR:-/tmp}/tsugumi-image.XXXXXX")"
trap 'rm -rf "$stage"' EXIT
# Exactly the files Git would ship (tracked + untracked, not ignored).
(cd "$repo" && git ls-files -z -co --exclude-standard | xargs -0 -I{} rsync -R "{}" "$stage/")
(cd "$stage" && git init -q && git add -A && git -c user.email=b@t -c user.name=b commit -qm build)

container volume inspect tsugumi-builder-nix >/dev/null 2>&1 || container volume create -s 40G tsugumi-builder-nix >/dev/null
container run --rm --name tsugumi-image-builder -c "${TSUGUMI_BUILDER_CPUS:-8}" -m "${TSUGUMI_BUILDER_MEMORY:-16G}" \
  -v tsugumi-builder-nix:/cache -v "$stage:/src:ro" -v "$out:/out" \
  "$TSUGUMI_BUILDER_IMAGE" sh -euc '
    # Chroot store under /cache: persistent across builds, separate from the image store.
    nixf() { nix --extra-experimental-features "nix-command flakes" --store /cache "$@"; }
    img=$(nixf build --no-link --print-out-paths -L "git+file:///src#packages.aarch64-linux.'"$attr"'" 2>&1 | grep -v "^copying path" | tail -40 | tee /dev/stderr | tail -1)
    [ "'"$attr"'" = image ] || exit 0
    cp /cache$img /out/tsugumi-env.tar.gz
    # OCI layout for `container image load`, converted by skopeo run in the store namespace.
    nixf shell nixpkgs#skopeo -c skopeo --insecure-policy copy       "docker-archive:/out/tsugumi-env.tar.gz" "oci-archive:/out/tsugumi-env.tar:tsugumi-env:'"$TSUGUMI_IMAGE_TAG"'"
  '
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
