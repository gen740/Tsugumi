#!/usr/bin/env bash
# SwiftPM rejects swiftSettings on plugin targets. Check only our plugin with
# warnings as errors, without changing the compilation of dependency plugins.
set -euo pipefail
repo="$(cd "$(dirname "$0")/../../.." && pwd)"
compiler="$(/usr/bin/xcrun --find swiftc)"
plugin_api="$(dirname "$compiler")/../lib/swift/pm/PluginAPI"
sdk="$(/usr/bin/xcrun --sdk macosx --show-sdk-path)"
"$compiler" -typecheck -parse-as-library -swift-version 6 \
  -package-description-version 6.2.0 -warnings-as-errors \
  -sdk "$sdk" \
  -module-cache-path "$repo/ui/macos/.build/plugin-warning-modules" \
  -I "$plugin_api" "$repo/client/swift/Plugins/FlatcPlugin/plugin.swift"
