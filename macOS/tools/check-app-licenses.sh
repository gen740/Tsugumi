#!/usr/bin/env bash
# Checks a built Tsugumi.app before it is published: its licenses are in it, for
# the packages it pins and the boot disk it carries, that disk's source is
# published, and the only math font it carries is the one it draws with.
#
#   macOS/tools/check-app-licenses.sh dist/Tsugumi.app
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
res="${1:?usage: check-app-licenses.sh <app>}/Contents/Resources"
fail=0
need() {
  [ -s "$res/$1" ] || { echo "missing from the app: $1" >&2; fail=1; }
}

for f in licenses/LICENSE licenses/THIRD_PARTY_NOTICES.md \
  licenses/dawn/dawn.LICENSE licenses/dawn/abseil-cpp.LICENSE licenses/dawn/webgpu-headers.LICENSE licenses/dawn/chromium.LICENSE \
  licenses/tree-sitter/LICENSE.tree-sitter licenses/tree-sitter/tree-sitter-nix.LICENSE licenses/tree-sitter/tree-sitter-python.LICENSE \
  licenses/fonts/latin-modern-math/doc/GUST-FONT-LICENSE.txt licenses/fonts/latin-modern-math/doc/MANIFEST-Latin-Modern-Math.txt \
  licenses/fonts/latin-modern-math/otf/latinmodern-math.otf licenses/fonts/YuseiMagic-OFL.txt \
  licenses/plot-viewer/README.md licenses/vm-boot/inventory.json licenses/vm-boot/kernel-config \
  tsugumi-source/LICENSE tsugumi-source/THIRD_PARTY_NOTICES.md; do
  need "$f"
done

# Every package the app pins.
for p in $(python3 -c 'import json, sys; print(*[p["location"].rstrip("/").removesuffix(".git").rsplit("/", 1)[1] for p in json.load(open(sys.argv[1]))["pins"]])' "$repo/macOS/Package.resolved"); do
  [ -n "$(ls -A "$res/licenses/swift-packages/$p" 2>/dev/null)" ] || { echo "no license of the package $p" >&2; fail=1; }
done

# The list of what is on the boot disk is that of the disk in this app.
vm="$(cat "$res/tsugumi-vm/vm-version")"
listed="$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["vm_version"])' "$res/licenses/vm-boot/inventory.json")"
[ "$vm" = "$listed" ] || { echo "the app's boot disk is $vm, third-party/vm-boot describes $listed: run macOS/tools/vm-inventory.py" >&2; fail=1; }

# The corresponding source of that disk is published.
source_url="https://github.com/gen740/tsugumi-vm/releases/download/${vm%%-*}/sources.tar.gz"
curl -fsIL -o /dev/null "$source_url" || { echo "the source of the boot disk is not at $source_url: run macOS/tools/publish-vm.sh" >&2; fail=1; }

# Math fonts: only SwiftMath's default (build-app.sh removes the others).
fonts="$(find "$res/SwiftMath_SwiftMath.bundle" -name '*.otf' -exec basename {} \;)"
[ "$fonts" = latinmodern-math.otf ] || { echo "math fonts in the app: $fonts" >&2; fail=1; }

[ "$fail" = 0 ]
echo "the app's licenses are complete"
