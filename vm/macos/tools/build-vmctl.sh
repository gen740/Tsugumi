#!/usr/bin/env bash
# Build the Host RPC diagnostic client and its independently owned VM helper.
set -euo pipefail
repo="$(cd "$(dirname "$0")/../../.." && pwd)"
flatc_bin="$("$repo/ui/macos/tools/flatc-bin.sh")"
for package in cli/macos vm/macos/runtime; do
  product=tsugumi-vmctl
  [ "$package" != vm/macos/runtime ] || product=tsugumi-vmhost
  (cd "$repo/$package" && env -i HOME="$HOME" TSUGUMI_CLIENT_LIB_DIR="${TSUGUMI_CLIENT_LIB_DIR:-$repo/build/client}" PATH="$flatc_bin:/usr/bin:/bin:/usr/sbin:/sbin" swift build --product "$product")
done
bin="$repo/cli/macos/.build/debug/tsugumi-vmctl"
cp "$repo/vm/macos/runtime/.build/debug/tsugumi-vmhost" "$(dirname "$bin")/tsugumi-vmhost"
codesign --force --sign - --entitlements "$repo/vm/macos/Resources/Tsugumi.entitlements" "$(dirname "$bin")/tsugumi-vmhost"
codesign --force --sign - "$bin"
echo "$bin"
