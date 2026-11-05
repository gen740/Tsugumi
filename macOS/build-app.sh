#!/usr/bin/env bash
# Builds dist/Tsugumi.app: the SwiftUI app, the Dawn renderer, the bundled
# demos, the shared VM's boot disk (macOS/Container/vm-boot.nix) and the source
# the VM builds Tsugumi from on its first boot (macOS/Container/vm-source.txt).
# Tools (flatc, cmake, ninja) are taken from the flake's nixpkgs; compilers
# are Apple's.
#
#   macOS/build-app.sh            # dev build (optimized; "Tsugumi Dev")
#   macOS/build-app.sh --release  # the build the release workflow publishes
#
# The two are different apps to macOS (AppChannel.swift): the dev build has
# the bundle id app.tsugumi.Tsugumi.dev, keeps its VM and store in
# ~/Library/Application Support/Tsugumi-dev and never updates itself; the
# release (app.tsugumi.Tsugumi, Application Support/Tsugumi) updates itself
# from the GitHub releases (Updater.swift). Checks with a dev build never
# touch the VM of the release in use.
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
channel=dev
case "${1:-}" in
  --release) channel=release ;;
  "") ;;
  *) sed -n '8,9s/^# \{0,1\}//p' "$0" >&2; exit 2 ;;
esac
if [ "$channel" = release ]; then
  bundle_id=app.tsugumi.Tsugumi
  name=Tsugumi
else
  bundle_id=app.tsugumi.Tsugumi.dev
  name="Tsugumi Dev"
fi
dist="$repo/dist"
# The version (VERSION, see docs/Development.md) and a build number that only grows.
version="$(cat "$repo/VERSION")"
build="$(git -C "$repo" rev-list --count HEAD 2>/dev/null || echo 1)"
app="$dist/Tsugumi.app"
renderer="$repo/build-renderer/ui/renderer/libtsugumi_renderer.dylib"

[ -f "$renderer" ] || "$repo/ui/renderer/build.sh"
vm="$dist/tsugumi-vm"
[ -f "$vm/vmlinux" ] && [ -f "$vm/rootfs.erofs" ] || "$repo/macOS/Container/build-image.sh" --vm

# Swift uses the Apple toolchain with a clean environment; only flatc comes from Nix.
flatc_bin="$(nix build --no-link --print-out-paths --inputs-from "$repo" nixpkgs#flatbuffers)/bin"
swiftenv=(env -i HOME="$HOME" PATH="$flatc_bin:/usr/bin:/bin:/usr/sbin:/sbin")
# Swift Build, also where it is not the default yet (Xcode 26.6 of the release
# runner), for Apple silicon only (the renderer is arm64): its Bundle.module finds the packages' resource bundles in
# Contents/Resources. The native build system's looks only next to the app
# and in the build folder, so its app crashes on any other Mac.
build_opts=(-c release --build-system swiftbuild --arch arm64)
(cd "$repo/macOS" && "${swiftenv[@]}" swift build "${build_opts[@]}" --product Tsugumi)
bin="$(cd "$repo/macOS" && "${swiftenv[@]}" swift build "${build_opts[@]}" --show-bin-path)"

rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Frameworks" "$app/Contents/Resources"
cp "$bin/Tsugumi" "$app/Contents/MacOS/Tsugumi"
# Nothing may look for files where this machine built them (that only works here).
if strings "$app/Contents/MacOS/Tsugumi" | grep -F "$repo/" | grep -v '\.swift$' | head -3 | grep .; then
  echo "the app refers to paths of this build (above)" >&2
  exit 1
fi
cp "$renderer" "$app/Contents/Frameworks/"
for b in "$bin"/*.bundle; do cp -R "$b" "$app/Contents/Resources/"; done
# SwiftMath's bundle has 12 math fonts, each with its own terms; the app draws
# with one, the default of MTMathUILabel, loaded on demand. The others stay out.
math_fonts="$app/Contents/Resources/SwiftMath_SwiftMath.bundle/Contents/Resources/mathFonts.bundle"
[ -f "$math_fonts/latinmodern-math.otf" ]
find "$math_fonts" -type f ! -name 'latinmodern-math.*' ! -name LICENSE ! -name GUST-FONT-LICENSE.txt -delete
cp -R "$repo/demos" "$app/Contents/Resources/demos"
# The module viewer (ui/modules) and the plot libraries it loads: the web UI's
# pinned npm packages (web/package-lock.json), the same files the web UI serves.
[ -d "$repo/web/node_modules/plotly.js-dist-min" ] || nix develop "$repo" -c npm ci --prefix "$repo/web" --ignore-scripts
mods="$app/Contents/Resources/modules"
mkdir -p "$mods"
cp "$repo/ui/modules/viewer.html" "$repo/ui/modules/viewer.js" \
  "$repo/web/node_modules/plotly.js-dist-min/plotly.min.js" \
  "$repo/web/node_modules/vega/build/vega.min.js" \
  "$repo/web/node_modules/vega-lite/build/vega-lite.min.js" \
  "$repo/web/node_modules/vega-embed/build/vega-embed.min.js" "$mods/"
for p in plotly.js-dist-min:plotly.js vega:vega vega-lite:vega-lite vega-embed:vega-embed; do
  cp "$repo/web/node_modules/${p%%:*}/LICENSE" "$mods/LICENSE.${p#*:}.txt"
done
# What the in-app Agent may read as references (reference_read).
mkdir -p "$app/Contents/Resources/reference"
cp "$repo/agent/Reference/card-guide.md" "$app/Contents/Resources/reference/card-guide.md"
cp "$repo/sdk/python/tsugumi/__init__.py" "$app/Contents/Resources/reference/tsugumi-sdk.py"
# Help > Tsugumi を知る: the documentation site's story pages, as they are, with
# what they use (Sources/App/Explainer/ExplainerWindow.swift).
explainer="$app/Contents/Resources/explainer"
mkdir -p "$explainer/pages" "$explainer/site/js" "$explainer/site/assets" "$explainer/assets"
stories=$(grep -l 'class="story"' "$repo"/docs/pages/*.html)
[ -n "$stories" ] || { echo "no story pages in docs/pages" >&2; exit 1; }
cp $stories "$explainer/pages/"
cp "$repo/docs/site/site.css" "$explainer/site/"
cp "$repo"/docs/site/js/{reader,art,scenes,scenes2}.js "$explainer/site/js/"
cp "$repo"/docs/site/assets/* "$explainer/site/assets/"
# The images the pages name, and no others: docs/assets holds all the screenshots.
for used in $(grep -oh 'src="\.\./assets/[^"]*"\|href="\.\./assets/[^"]*"' $stories | sed 's/.*"\.\.\/\(.*\)"/\1/' | sort -u); do
  mkdir -p "$explainer/$(dirname "$used")"
  cp "$repo/docs/$used" "$explainer/$used"
done
# UI strings: Japanese source strings, English table generated from tools/en_strings.py.
python3 "$repo/macOS/Resources/tools/en_strings.py" >/dev/null
cp -R "$repo/macOS/Resources/ja.lproj" "$repo/macOS/Resources/en.lproj" "$app/Contents/Resources/"
# The shared VM: copied to the support folder on first start (APFS clone).
cp -R "$vm" "$app/Contents/Resources/tsugumi-vm"
# Tsugumi's source: the VM builds Tsugumi from it (vm-system.nix).
src="$app/Contents/Resources/tsugumi-source"
mkdir -p "$src"
grep -v '^#' "$repo/macOS/Container/vm-source.txt" | while read -r entry; do
  [ -n "$entry" ] || continue
  mkdir -p "$src/$(dirname "$entry")"
  cp -R "$repo/$entry" "$src/$entry"
done
# The licenses of all of the above, and the boot disk's from its release
# (publish-vm.sh, with its source). A release needs them; a disk not published
# yet can still be tried in a dev build.
"$repo/macOS/tools/bundle-licenses.sh" "$app/Contents/Resources/licenses"
vm_licenses="https://github.com/${TSUGUMI_VM_RELEASES:-gen740/tsugumi-vm}/releases/download/$(cut -d- -f1 "$vm/vm-version")/licenses.tar.gz"
if ! curl -fsSL "$vm_licenses" | tar -xz -C "$app/Contents/Resources/licenses"; then
  [ "$channel" != release ] || { echo "no licenses at $vm_licenses: run macOS/tools/publish-vm.sh" >&2; exit 1; }
  echo "warning: no licenses of the boot disk at $vm_licenses" >&2
fi
# App icon: generated from docs/assets/tsugumi-icon.svg by tools/make_icon.swift.
cp "$repo/macOS/Resources/AppIcon.icns" "$app/Contents/Resources/AppIcon.icns"
cat > "$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>$name</string>
  <key>CFBundleDisplayName</key><string>$name</string>
  <key>CFBundleIdentifier</key><string>$bundle_id</string>
  <key>TsugumiChannel</key><string>$channel</string>
  <key>CFBundleExecutable</key><string>Tsugumi</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$version</string>
  <key>CFBundleVersion</key><string>$build</string>
  <key>CFBundleDevelopmentRegion</key><string>ja</string>
  <key>CFBundleLocalizations</key><array><string>ja</string><string>en</string></array>
  <key>LSMinimumSystemVersion</key><string>26.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSPrincipalClass</key><string>NSApplication</string>
</dict>
</plist>
PLIST
# Ad-hoc signature (no Developer ID, no notarization), with the
# virtualization entitlement the shared VM needs. Nested code first, each by
# itself (not --deep, which signs whatever it finds the same way).
# Rpaths of the machine it was built on (where the renderer was built) are not
# the app's: it finds its libraries in its own Frameworks.
otool -l "$app/Contents/MacOS/Tsugumi" | awk '/LC_RPATH/ {getline; getline; print $2}' | grep '^/' | while read -r p; do
  install_name_tool -delete_rpath "$p" "$app/Contents/MacOS/Tsugumi"
done
for lib in "$app/Contents/Frameworks/"*.dylib; do codesign --force --sign - "$lib" >/dev/null; done
codesign --force --sign - --entitlements "$repo/macOS/Resources/Tsugumi.entitlements" "$app" >/dev/null
# The bundle was made anew: Finder and the Dock read its icon again.
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$app" >/dev/null 2>&1 || true
touch "$app"
echo "$app"
