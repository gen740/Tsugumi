#!/usr/bin/env bash
# Writes the licenses the app distributes into a folder (build-app.sh:
# Contents/Resources/licenses), from the files of the very checkouts it was
# built from. Anything missing stops the build.
#
#   macOS/tools/bundle-licenses.sh <folder>
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
dest="${1:?usage: bundle-licenses.sh <folder>}"
dawn="$repo/build-renderer/_deps/dawn-src"
rm -rf "$dest"
mkdir -p "$dest"

cp "$repo/LICENSE" "$repo/THIRD_PARTY_NOTICES.md" "$dest/"

# SwiftPM packages: every one Package.resolved pins has a checkout here.
for checkout in "$repo/macOS/.build/checkouts/"*; do
  to="$dest/swift-packages/$(basename "$checkout")"
  mkdir -p "$to"
  find "$checkout" -maxdepth 1 -type f \( -iname 'LICENSE*' -o -iname 'COPYING*' -o -iname 'NOTICE*' \) -exec cp {} "$to/" \;
  [ -n "$(ls -A "$to")" ] || { echo "no license file in $checkout" >&2; exit 1; }
done

# Grammars and highlight queries copied into this repository.
mkdir -p "$dest/tree-sitter"
cp "$repo/macOS/Sources/Grammars/LICENSE.tree-sitter" "$repo/macOS/Sources/Grammars/README.md" \
  "$repo/macOS/Sources/App/Resources/TreeSitter/SOURCES.md" "$dest/tree-sitter/"
cp "$repo/macOS/Sources/Grammars/TreeSitterNix/LICENSE" "$dest/tree-sitter/tree-sitter-nix.LICENSE"
cp "$repo/macOS/Sources/Grammars/TreeSitterPython/LICENSE" "$dest/tree-sitter/tree-sitter-python.LICENSE"

# What the renderer's static Dawn library compiles (docs/LicenseAudit.md).
mkdir -p "$dest/dawn"
cp "$dawn/LICENSE" "$dest/dawn/dawn.LICENSE"
cp "$dawn/third_party/abseil-cpp/LICENSE" "$dest/dawn/abseil-cpp.LICENSE"
cp "$dawn/third_party/webgpu-headers/src/LICENSE" "$dest/dawn/webgpu-headers.LICENSE"
cp "$repo/third-party/chromium/LICENSE" "$dest/dawn/chromium.LICENSE"

# Fonts. Latin Modern Math: its complete upstream package, as nixpkgs fetches
# it (the font in SwiftMath's bundle is byte for byte the one in it).
mkdir -p "$dest/fonts"
lm="$(nix build --no-link --print-out-paths --inputs-from "$repo" nixpkgs#lmmath.src)"
cp -R "$lm" "$dest/fonts/latin-modern-math"
chmod -R u+w "$dest/fonts/latin-modern-math"
cp "$repo/macOS/Resources/explainer/YuseiMagic-OFL.txt" "$dest/fonts/"

# The plot viewer's libraries and what they were built from (web/package-lock.json).
nix develop "$repo" -c node "$repo/web/scripts/licenses.mjs" viewer "$dest/plot-viewer"

# The VM's boot disk (macOS/tools/vm-inventory.py).
cp -R "$repo/third-party/vm-boot" "$dest/vm-boot"
