#!/usr/bin/env bash
# Builds dist/Tsugumi.app: the SwiftUI app, the bundled
# demos, the built Web UI (the hostd the app starts serves it) and the
# release-built guest archive.
# Tools (flatc, cmake, ninja) are taken from the flake's nixpkgs; compilers
# are Apple's.
#
#   ui/macos/build-app.sh            # dev build (optimized; "Tsugumi Dev")
#   ui/macos/build-app.sh --release  # the build the release workflow publishes
#   TSUGUMI_CODESIGN_IDENTITY="Apple Development" ui/macos/build-app.sh
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
repo="$(cd "$(dirname "$0")/../.." && pwd)"
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
# The guest image from its artifact (nix/guest-image.nix); a release takes
# only the one built for HEAD.
guest_args=()
[ "$channel" != release ] || guest_args=(--release)
(cd "$repo" && nix run .#guest-image -- ${guest_args[@]+"${guest_args[@]}"})
# The Web UI, built reproducibly from the pinned Nix package (as for the command's archive).
web_out="$(cd "$repo" && nix build --no-link --print-out-paths '.#web^out')"

# The native host service and RPC client are shared by both UI frontends.
"$repo/hostd/tools/build-macos.sh"

# Swift uses the Apple toolchain with a clean environment; only flatc comes from Nix.
flatc_bin="$(cd "$repo" && nix build --no-link --print-out-paths .#flatc)/bin"
swiftenv=(env -i HOME="$HOME" TSUGUMI_CLIENT_LIB_DIR="$repo/build-host/client" PATH="$flatc_bin:/usr/bin:/bin:/usr/sbin:/sbin")
# Swift Build, also where it is not the default yet (Xcode 26.6 of the release
# runner), for Apple silicon only (the shared VM is arm64): its Bundle.module finds the packages' resource bundles in
# Contents/Resources. The native build system's looks only next to the app
# and in the build folder, so its app crashes on any other Mac.
build_opts=(-c release --build-system swiftbuild --arch arm64)
"${swiftenv[@]}" bash "$repo/ui/macos/tools/check-plugin-warnings.sh"
(cd "$repo/ui/macos" && "${swiftenv[@]}" swift build "${build_opts[@]}" --product Tsugumi)
(cd "$repo/vm/macos/runtime" && "${swiftenv[@]}" swift build "${build_opts[@]}" --product tsugumi-vmhost)
bin="$(cd "$repo/ui/macos" && "${swiftenv[@]}" swift build "${build_opts[@]}" --show-bin-path)"

rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources" "$app/Contents/Frameworks"
cp "$repo/build-host/Frameworks/"*.dylib "$app/Contents/Frameworks/"
cp "$bin/Tsugumi" "$app/Contents/MacOS/Tsugumi"
wasmtime="$app/Contents/Frameworks/libwasmtime.dylib"
cp -L "$repo/build-host/client/libwasmtime.dylib" "$wasmtime"
chmod u+w "$wasmtime"
wasmtime_id=$(/usr/bin/otool -D "$wasmtime" | tail -n 1)
libiconv_id=$(/usr/bin/otool -L "$wasmtime" | awk '$1 ~ /^\/nix\/store\/.*\/libiconv\.2\.dylib$/ {print $1}')
wasmtime_changes=()
if [ -n "$libiconv_id" ]; then
  wasmtime_changes=(-change "$libiconv_id" /usr/lib/libiconv.2.dylib)
fi
/usr/bin/install_name_tool -id @rpath/libwasmtime.dylib ${wasmtime_changes[@]+"${wasmtime_changes[@]}"} "$wasmtime"
/usr/bin/install_name_tool -change "$wasmtime_id" @rpath/libwasmtime.dylib "$app/Contents/MacOS/Tsugumi"
frameworks_rpath=@executable_path/../Frameworks
if ! /usr/bin/otool -l "$app/Contents/MacOS/Tsugumi" | awk '$1 == "path" {print $2}' | grep -Fx "$frameworks_rpath" >/dev/null; then
  /usr/bin/install_name_tool -add_rpath "$frameworks_rpath" "$app/Contents/MacOS/Tsugumi"
fi
for binary in "$app/Contents/MacOS/Tsugumi" "$app/Contents/Frameworks/"*.dylib; do
  if /usr/bin/otool -L "$binary" | /usr/bin/grep -q '/nix/store/'; then
    echo "The distributable binary still depends on the Nix store: $binary" >&2
    exit 1
  fi
done
vm_bin="$(cd "$repo/vm/macos/runtime" && "${swiftenv[@]}" swift build "${build_opts[@]}" --show-bin-path)"
cp "$vm_bin/tsugumi-vmhost" "$repo/build-host/hostd/tsugumi-hostd" "$app/Contents/MacOS/"
cp "$repo/build-host/generated/bfbs/"{agent,host}.bfbs "$app/Contents/Resources/"

# Nothing may look for files where this machine built them (that only works here).
if strings "$app/Contents/MacOS/Tsugumi" | grep -F "$repo/" | grep -v '\.swift$' | head -3 | grep .; then
  echo "the app refers to paths of this build (above)" >&2
  exit 1
fi
for b in "$bin"/*.bundle; do cp -R "$b" "$app/Contents/Resources/"; done
# Keep only Latin Modern Math, the default font loaded by MTMathUILabel.
math_fonts="$app/Contents/Resources/SwiftMath_SwiftMath.bundle/Contents/Resources/mathFonts.bundle"
[ -f "$math_fonts/latinmodern-math.otf" ]
find "$math_fonts" -type f ! -name 'latinmodern-math.*' -delete
cp "$repo/ui/macos/Resources/Fonts/latin-modern-math/latinmodern-math.otf" "$math_fonts/"
licenses="$app/Contents/Resources/licenses"
mkdir -p "$licenses"
cp -R "$repo/ui/macos/third-party/". "$repo/third-party/". "$licenses/"
mkdir -p "$licenses/latin-modern-math"
cp "$repo/ui/macos/Resources/Fonts/latin-modern-math/GUST-FONT-LICENSE.txt" "$licenses/latin-modern-math/"
for grammar in TreeSitterNix TreeSitterPython TreeSitterJuliaScanner; do
  mkdir -p "$licenses/$grammar"
  cp "$repo/ui/macos/Sources/Grammars/$grammar/LICENSE" "$licenses/$grammar/"
done
cp "$repo/ui/macos/Sources/Grammars/LICENSE.tree-sitter" "$licenses/"
cp -R "$repo/hostd/formatters/third-party" "$licenses/formatters"
cp -R "$repo/build-host/Resources/formatters" "$app/Contents/Resources/formatters"
cp -R "$repo/build-host/Resources/git" "$app/Contents/Resources/git"
cp -R "$repo/build-host/Resources/git-licenses" "$licenses/git"
cp -R "$repo/build-host/Resources/view-licenses/". "$licenses/"
cp -R "$web_out/share/tsugumi/demos" "$app/Contents/Resources/demos"
chmod -R u+w "$app/Contents/Resources/demos"
# The page hostd serves (CoreHostClient passes it as --web-root) and its licences.
cp -R "$web_out/share/tsugumi/ui/web/dist/public" "$app/Contents/Resources/web"
mkdir -p "$licenses/web"
cp -R "$web_out/share/tsugumi/ui/web/dist/licenses/". "$licenses/web/"
chmod -R u+w "$app/Contents/Resources/web" "$licenses/web"
# Japanese source strings and the generated English table.
resources="$app/Contents/Resources"
cp -R "$repo/ui/macos/Resources/ja.lproj" "$resources/"
mkdir -p "$resources/en.lproj"
python3 "$repo/ui/shared/i18n/strings.py" > "$resources/en.lproj/Localizable.strings"
# The app carries one guest payload; the helper extracts it in private support.
mkdir -p "$app/Contents/Resources/tsugumi-vm"
cp "$vm/tsugumi-vm.tar.gz" "$vm/vm-manifest.json" "$app/Contents/Resources/tsugumi-vm/"
# App icon: generated from docs/assets/tsugumi-icon.svg by tools/make_icon.swift.
cp "$repo/ui/macos/Resources/AppIcon.icns" "$app/Contents/Resources/AppIcon.icns"
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
# as codesign takes it), giving the app a stable designated requirement across
# rebuilds. The release build ignores it: the workflow's artifact is
# ad-hoc whatever the environment holds.
# Nested code first, each by itself (not --deep, which signs whatever it finds
# the same way).
identity=-
if [ "$channel" = dev ] && [ -n "${TSUGUMI_CODESIGN_IDENTITY:-}" ]; then
  identity="$TSUGUMI_CODESIGN_IDENTITY"
fi
for library in "$app/Contents/Frameworks/"*.dylib; do
  /usr/bin/strip -S "$library"
  codesign --force --sign "$identity" "$library" >/dev/null
done
for tool in "$app/Contents/Resources/"{formatters,git}/*; do
  codesign --force --sign "$identity" "$tool" >/dev/null
done
for binary in "$app/Contents/MacOS/"*; do
  /usr/bin/strip -S "$binary"
done
codesign --force --sign "$identity" "$app/Contents/MacOS/tsugumi-hostd" >/dev/null
codesign --force --sign "$identity" --entitlements "$repo/vm/macos/Resources/Tsugumi.entitlements" "$app/Contents/MacOS/tsugumi-vmhost" >/dev/null
codesign --force --sign "$identity" --entitlements "$repo/vm/macos/Resources/Tsugumi.entitlements" "$app" >/dev/null
# The bundle was made anew: Finder and the Dock read its icon again.
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$app" >/dev/null 2>&1 || true
touch "$app"
echo "$app"
