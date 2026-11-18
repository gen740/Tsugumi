#!/bin/sh
# Run in the Apple container by `nix run .#guest-image -- --build` (nix/guest-image.nix),
# with the flake-pinned Nix and Python on PATH.
# Keep the bootstrap container store separate from the persistent build store.
set -eu
export NIX_REMOTE='local?root=/cache'
img=$(nix --accept-flake-config build --no-link --print-out-paths -L "path:/src#packages.aarch64-linux.$GUEST_ATTRIBUTE")
if [ "$GUEST_ATTRIBUTE" = kernel ]; then cp "/cache$img/vmlinux" /out/tsugumi-kernel; fi
if [ "$GUEST_ATTRIBUTE" = vm ]; then
  python3 /src/vm/guest/tools/vm-artifact.py build "/cache$img" /out --revision "$GUEST_REVISION"
fi
