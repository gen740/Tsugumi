#!/bin/sh
# Invoked with the flake-pinned Nix/Python/tools on PATH by build-image.sh.
# Keep the bootstrap container store separate from the persistent build store.
set -eu
export NIX_REMOTE='local?root=/cache' TSUGUMI_BUILD_STORE_ROOT=/cache
img=$(nix --accept-flake-config build --no-link --print-out-paths -L "path:/src#packages.aarch64-linux.$GUEST_ATTRIBUTE")
if [ "$GUEST_ATTRIBUTE" = kernel ]; then cp "/cache$img/vmlinux" /out/tsugumi-kernel; fi
if [ "$GUEST_ATTRIBUTE" = vm ]; then
  python3 /src/macOS/tools/vm-artifact.py build "/cache$img" /out --revision "$GUEST_REVISION"
fi
