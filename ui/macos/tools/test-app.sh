#!/usr/bin/env bash
set -euo pipefail
repo="$(cd "$(dirname "$0")/../../.." && pwd)"
keys="$repo/ui/macos/.build/localization"
mkdir -p "$keys"
/usr/bin/swift test --package-path "$repo/ui/macos" \
  -Xswiftc -emit-localized-strings -Xswiftc -emit-localized-strings-path -Xswiftc "$keys"
python3 "$repo/ui/shared/i18n/strings.py" --check "$keys"
