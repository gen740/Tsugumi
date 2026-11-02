#!/usr/bin/env bash
# Builds dist/Tsugumi.app: the SwiftUI app, the Dawn renderer, the bundled
# demos and the environment image for the first start. Run inside `nix develop`
# (flatc, cmake and ninja come from the flake).
#
#   macOS/build-app.sh            # release build
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
dist="$repo/dist"
app="$dist/Tsugumi.app"
renderer="$repo/build-renderer/ui/renderer/libtsugumi_renderer.dylib"

[ -f "$renderer" ] || "$repo/ui/renderer/build.sh"
image="$dist/tsugumi-env.tar"
[ -f "$image" ] || TSUGUMI_IMAGE_OUT="$image" "$repo/macOS/Container/build-image.sh"

# Swift must use the Apple toolchain, not the Nix compiler wrapper.
(cd "$repo/macOS" && env -u CC -u CXX -u SDKROOT -u NIX_CFLAGS_COMPILE -u NIX_LDFLAGS -u MACOSX_DEPLOYMENT_TARGET \
  -u DEVELOPER_DIR swift build -c release --product Tsugumi)
bin="$(cd "$repo/macOS" && env -u SDKROOT swift build -c release --show-bin-path)"

rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Frameworks" "$app/Contents/Resources"
cp "$bin/Tsugumi" "$app/Contents/MacOS/Tsugumi"
cp "$renderer" "$app/Contents/Frameworks/"
for b in "$bin"/*.bundle; do cp -R "$b" "$app/Contents/Resources/"; done
cp -R "$repo/demos" "$app/Contents/Resources/demos"
cp "$image" "$app/Contents/Resources/tsugumi-env.tar"
cat > "$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>Tsugumi</string>
  <key>CFBundleDisplayName</key><string>Tsugumi</string>
  <key>CFBundleIdentifier</key><string>app.tsugumi.Tsugumi</string>
  <key>CFBundleExecutable</key><string>Tsugumi</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>1.0.0</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSMinimumSystemVersion</key><string>26.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSPrincipalClass</key><string>NSApplication</string>
</dict>
</plist>
PLIST
# Local use only: ad-hoc signature (no notarization).
codesign --force --deep --sign - "$app" >/dev/null
echo "$app"
