#!/usr/bin/env bash
# Builds dist/Tsugumi.app: the SwiftUI app, the bundled
# demos, the shared VM's boot disk (macOS/Container/vm-boot.nix) and the source
# the VM builds Tsugumi from on its first boot (macOS/Container/vm-source.txt).
# Tools (flatc, cmake, ninja) are taken from the flake's nixpkgs; compilers
# are Apple's.
#
#   macOS/build-app.sh            # dev build (optimized; "Tsugumi Dev")
#   macOS/build-app.sh --release  # the build the release workflow publishes
#   TSUGUMI_CODESIGN_IDENTITY="Apple Development" macOS/build-app.sh
#                                 # dev build signed with a certificate of yours
#                                 # (see "Signing" below)
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
# The version (VERSION, see RELEASING.md) and a build number that only grows.
version="$(cat "$repo/VERSION")"
build="$(git -C "$repo" rev-list --count HEAD 2>/dev/null || echo 1)"
app="$dist/Tsugumi.app"
vm="$dist/tsugumi-vm"
[ -f "$vm/vmlinux" ] && [ -f "$vm/rootfs.erofs" ] || "$repo/macOS/Container/build-image.sh" --vm

# Native core Agent and RPC client are shared by both UI frontends.
"$repo/macOS/tools/build-agent.sh"

# Swift uses the Apple toolchain with a clean environment; only flatc comes from Nix.
flatc_bin="$("$repo/macOS/tools/flatc-bin.sh")"
swiftenv=(env -i HOME="$HOME" TSUGUMI_CLIENT_LIB_DIR="$repo/build-host/client" PATH="$flatc_bin:/usr/bin:/bin:/usr/sbin:/sbin")
# Swift Build, also where it is not the default yet (Xcode 26.6 of the release
# runner), for Apple silicon only (the shared VM is arm64): its Bundle.module finds the packages' resource bundles in
# Contents/Resources. The native build system's looks only next to the app
# and in the build folder, so its app crashes on any other Mac.
build_opts=(-c release --build-system swiftbuild --arch arm64)
(cd "$repo/macOS" && "${swiftenv[@]}" swift build "${build_opts[@]}" --product Tsugumi)
(cd "$repo/macOS" && "${swiftenv[@]}" swift build "${build_opts[@]}" --product tsugumi-vmhost)
bin="$(cd "$repo/macOS" && "${swiftenv[@]}" swift build "${build_opts[@]}" --show-bin-path)"

rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp "$bin/Tsugumi" "$app/Contents/MacOS/Tsugumi"
cp "$bin/tsugumi-vmhost" "$repo/build-host/core/tsugumi-agentd" "$repo/build-host/client/tsugumi-rpc" "$app/Contents/MacOS/"
cp "$repo/build-host/generated/bfbs/agent.bfbs" "$app/Contents/Resources/"

# Nothing may look for files where this machine built them (that only works here).
if strings "$app/Contents/MacOS/Tsugumi" | grep -F "$repo/" | grep -v '\.swift$' | head -3 | grep .; then
  echo "the app refers to paths of this build (above)" >&2
  exit 1
fi
for b in "$bin"/*.bundle; do cp -R "$b" "$app/Contents/Resources/"; done
# SwiftMath's bundle has 12 math fonts, each with its own terms; the app draws
# with one, the default of MTMathUILabel, loaded on demand. The others stay out.
math_fonts="$app/Contents/Resources/SwiftMath_SwiftMath.bundle/Contents/Resources/mathFonts.bundle"
[ -f "$math_fonts/latinmodern-math.otf" ]
find "$math_fonts" -type f ! -name 'latinmodern-math.*' ! -name LICENSE ! -name GUST-FONT-LICENSE.txt -delete
cp -R "$repo/demos" "$app/Contents/Resources/demos"
# The common isolated frame host. Modules and libraries come from tsugumid over RPC.
mods="$app/Contents/Resources/modules"
mkdir -p "$mods"
cp "$repo/ui/modules/viewer.html" "$repo/ui/modules/viewer.js" "$mods/"
# What the in-app Agent may read as references (reference_read).
mkdir -p "$app/Contents/Resources/reference"
cp "$repo/agent/Reference/card-guide.md" "$app/Contents/Resources/reference/card-guide.md"
cp "$repo/sdk/python/tsugumi/__init__.py" "$app/Contents/Resources/reference/tsugumi-sdk.py"
# Help > Learn about Tsugumi: the documentation site's story pages, as they are, with
# what they use (Sources/App/Explainer/ExplainerWindow.swift).
(cd "$repo" && nix run .#build-doc)
bash "$repo/macOS/tools/bundle-docs.sh" "$app/Contents/Resources/explainer"
# Home's demo thumbnails (Sources/App/Home/DemoThumbnail.swift): the site's data of
# each demo and the images it names, without what only builds that data.
thumbs="$app/Contents/Resources/player-data"
mkdir -p "$thumbs/assets"
find "$repo/docs/site/player-data" -maxdepth 1 -name '*.json' ! -name index.json -exec cp {} "$thumbs/" \;
find "$repo/docs/site/player-data/assets" -type f ! -name '*.json' -exec cp {} "$thumbs/assets/" \;
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
# Signing, with the virtualization entitlement the shared VM needs. Ad-hoc by
# default (no Developer ID, no notarization). A dev build can take a certificate
# of the machine's keychain in TSUGUMI_CODESIGN_IDENTITY (a name or a SHA-1,
# as codesign takes it): the keychain then remembers the app by that certificate
# and its bundle id, not by the hash of one build, so it stops asking again
# after every build. The release build ignores it: the workflow's artifact is
# ad-hoc whatever the environment holds.
# Nested code first, each by itself (not --deep, which signs whatever it finds
# the same way).
identity=-
if [ "$channel" = dev ] && [ -n "${TSUGUMI_CODESIGN_IDENTITY:-}" ]; then
  identity="$TSUGUMI_CODESIGN_IDENTITY"
fi
codesign --force --sign "$identity" "$app/Contents/MacOS/tsugumi-agentd" >/dev/null
codesign --force --sign "$identity" "$app/Contents/MacOS/tsugumi-rpc" >/dev/null
codesign --force --sign "$identity" --entitlements "$repo/macOS/Resources/Tsugumi.entitlements" "$app/Contents/MacOS/tsugumi-vmhost" >/dev/null
codesign --force --sign "$identity" --entitlements "$repo/macOS/Resources/Tsugumi.entitlements" "$app" >/dev/null
# The bundle was made anew: Finder and the Dock read its icon again.
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$app" >/dev/null 2>&1 || true
touch "$app"
echo "$app"
