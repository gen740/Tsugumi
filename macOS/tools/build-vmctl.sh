#!/usr/bin/env bash
# Builds tsugumi-vmctl (tests drive the shared VM with it) and signs it with
# the virtualization entitlement.
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
flatc_bin="$("$repo/macOS/tools/flatc-bin.sh")"
(cd "$repo/macOS" && env -i HOME="$HOME" PATH="$flatc_bin:/usr/bin:/bin:/usr/sbin:/sbin" swift build --product tsugumi-vmctl)
bin="$repo/macOS/.build/debug/tsugumi-vmctl"
codesign --force --sign - --entitlements "$repo/macOS/Resources/Tsugumi.entitlements" "$bin"
echo "$bin"
