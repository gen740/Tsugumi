#!/usr/bin/env bash
# Build tsugumi-pack into dist/tsugumi-pack:
#
#   bin/tsugumi-pack                  the exporter and app launcher (signed, with
#                                     the virtualization entitlement)
#   share/tsugumi-pack/guest/         vmlinux, rootfs.erofs, identity
#
# The guest is packages.aarch64-linux.pack-guest. It needs an aarch64-linux
# Nix builder: a Linux host, a configured remote builder, or
#   TSUGUMI_PACK_LINUX="orb -m nixos"   a command that runs a Linux shell
#                                       seeing this checkout at the same path
# TSUGUMI_PACK_GUEST=DIR uses an already built guest instead.
# Signing: TSUGUMI_CODESIGN_IDENTITY (for example "Apple Development"), else ad hoc.
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
dist="$repo/dist/tsugumi-pack"
identity="${TSUGUMI_CODESIGN_IDENTITY:--}"
mkdir -p "$dist/bin" "$dist/share/tsugumi-pack"

guest="$dist/share/tsugumi-pack/guest"
if [ -n "${TSUGUMI_PACK_GUEST:-}" ]; then
  source_dir="$TSUGUMI_PACK_GUEST"
  copy=(cp)
else
  attr="path:$repo#packages.aarch64-linux.pack-guest"
  if [ -n "${TSUGUMI_PACK_LINUX:-}" ]; then
    # shellcheck disable=SC2086
    source_dir=$($TSUGUMI_PACK_LINUX bash -lc "nix --extra-experimental-features 'nix-command flakes' build --accept-flake-config --no-link --print-out-paths '$attr'")
    # shellcheck disable=SC2086
    copy=($TSUGUMI_PACK_LINUX cp)
  else
    source_dir=$(nix build --accept-flake-config --no-link --print-out-paths "$attr")
    copy=(cp)
  fi
fi
rm -rf "$guest.new"
mkdir -p "$guest.new"
for f in vmlinux rootfs.erofs identity; do "${copy[@]}" "$source_dir/$f" "$guest.new/$f"; done
chmod -R u+w "$guest.new"
rm -rf "$guest"
mv "$guest.new" "$guest"

(cd "$repo/pack/launcher" && env -i HOME="$HOME" PATH="/usr/bin:/bin:/usr/sbin:/sbin" swift build -c release --product tsugumi-pack)
cp "$repo/pack/launcher/.build/release/tsugumi-pack" "$dist/bin/tsugumi-pack"
entitlements="$(mktemp -t tsugumi-pack)"
trap 'rm -f "$entitlements"' EXIT
cat >"$entitlements" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>com.apple.security.virtualization</key><true/></dict></plist>
EOF
options=()
[ "$identity" = - ] || options=(--options runtime --timestamp=none)
codesign --force --sign "$identity" ${options[@]+"${options[@]}"} --entitlements "$entitlements" "$dist/bin/tsugumi-pack"
echo "$dist/bin/tsugumi-pack (guest $(cat "$guest/identity"))"
