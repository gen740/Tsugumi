#!/usr/bin/env bash
# Build tsugumi-pack into dist/tsugumi-pack:
#
#   bin/tsugumi-pack                  the exporter and app launcher (signed, with
#                                     the virtualization entitlement)
#   Frameworks/libwasmtime.dylib      native Dawn view runtime dependency
#   share/tsugumi-pack/guest/         vmlinux, export.erofs, run.erofs, identity
#
# The guest is packages.aarch64-linux.pack-guest. It needs an aarch64-linux
# Nix builder: a Linux host, a configured remote builder, or
#   TSUGUMI_PACK_LINUX="orb -m nixos"   a command that runs a Linux shell
#                                       seeing this checkout at the same path
# TSUGUMI_PACK_GUEST=DIR uses an already built guest instead.
# Signing: TSUGUMI_CODESIGN_IDENTITY (for example "Apple Development"), else ad hoc.
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
dist="$repo/dist/tsugumi-pack"
identity="${TSUGUMI_CODESIGN_IDENTITY:--}"
mkdir -p "$dist/bin" "$dist/share/tsugumi-pack"

guest="$dist/share/tsugumi-pack/guest"
if [ -n "${TSUGUMI_PACK_GUEST:-}" ]; then
  source_dir="$TSUGUMI_PACK_GUEST"
  copy=(cp)
else
  attr="path:$repo#packages.aarch64-linux.pack-guest"
  if [ -n "${TSUGUMI_PACK_LINUX:-}" ]; then
    # shellcheck disable=SC2086
    source_dir=$($TSUGUMI_PACK_LINUX bash -lc "nix --extra-experimental-features 'nix-command flakes' build --accept-flake-config --no-link --print-out-paths '$attr'")
    # shellcheck disable=SC2086
    copy=($TSUGUMI_PACK_LINUX cp)
  else
    source_dir=$(nix build --accept-flake-config --no-link --print-out-paths "$attr")
    copy=(cp)
  fi
fi
rm -rf "$guest.new"
mkdir -p "$guest.new"
for f in vmlinux export.erofs run.erofs identity; do "${copy[@]}" "$source_dir/$f" "$guest.new/$f"; done
chmod -R u+w "$guest.new"
rm -rf "$guest"
mv "$guest.new" "$guest"

# Build the native view host with Apple's compiler and the project's pinned
# Dawn, Wasmtime, and schema generator.
native="$repo/build-pack-host"
sdk=$(/usr/bin/xcrun --sdk macosx --show-sdk-path)
nix develop "$repo" -c bash -eu -c '
  repo=$1
  native=$2
  sdk=$3
  flatbuffers_root=$(cd "$(dirname "$(command -v flatc)")/.." && pwd)
  cmake -S "$repo" -B "$native" -G Ninja \
    -DTSUGUMI_BUILD_CORE=OFF -DTSUGUMI_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
    -DCMAKE_OSX_SYSROOT="$sdk" -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 \
    -Dflatbuffers_DIR="$flatbuffers_root/lib/cmake/flatbuffers" \
    -DTSUGUMI_CLIENT_CURL_LIBRARY="$sdk/usr/lib/libcurl.tbd"
  cmake --build "$native" --target tsugumi-view-host
  install -d "$native/view-licenses/dawn" "$native/view-licenses/wasmtime"
  install -m 644 "$TSUGUMI_DAWN_DIR/share/licenses/dawn/LICENSE" "$native/view-licenses/dawn/LICENSE"
  install -m 644 "$TSUGUMI_WASMTIME_LICENSE" "$native/view-licenses/wasmtime/LICENSE"
' pack-build "$repo" "$native" "$sdk"

swift_cmd=(env -i HOME="$HOME" PATH="/usr/bin:/bin:/usr/sbin:/sbin" TSUGUMI_CLIENT_LIB_DIR="$native/client" swift build -c release --package-path "$repo/pack/launcher")
"${swift_cmd[@]}" --product tsugumi-pack
cp "$("${swift_cmd[@]}" --show-bin-path)/tsugumi-pack" "$dist/bin/tsugumi-pack"
mkdir -p "$dist/Frameworks"
cp -R "$native/view-licenses" "$dist/share/tsugumi-pack/"
wasmtime="$dist/Frameworks/libwasmtime.dylib"
cp -L "$native/client/libwasmtime.dylib" "$wasmtime"
chmod u+w "$wasmtime"
wasmtime_id=$(/usr/bin/otool -D "$wasmtime" | tail -n 1)
libiconv_id=$(/usr/bin/otool -L "$wasmtime" | awk '$1 ~ /^\/nix\/store\/.*\/libiconv\.2\.dylib$/ {print $1}')
if [ -n "$libiconv_id" ]; then
  /usr/bin/install_name_tool -change "$libiconv_id" /usr/lib/libiconv.2.dylib "$wasmtime"
fi
/usr/bin/install_name_tool -id @rpath/libwasmtime.dylib "$wasmtime"
/usr/bin/install_name_tool -change "$wasmtime_id" @rpath/libwasmtime.dylib \
  -add_rpath @executable_path/../Frameworks "$dist/bin/tsugumi-pack"
for binary in "$dist/bin/tsugumi-pack" "$wasmtime"; do
  if /usr/bin/otool -L "$binary" | grep '/nix/store/' >/dev/null; then
    echo "The distributable binary still depends on the Nix store: $binary" >&2
    exit 1
  fi
done
codesign --force --sign "$identity" "$wasmtime"
entitlements="$(mktemp -t tsugumi-pack)"
trap 'rm -f "$entitlements"' EXIT
cat >"$entitlements" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>com.apple.security.virtualization</key><true/>
<key>com.apple.security.cs.allow-jit</key><true/></dict></plist>
EOF
options=()
[ "$identity" = - ] || options=(--options runtime --timestamp=none)
codesign --force --sign "$identity" ${options[@]+"${options[@]}"} --entitlements "$entitlements" "$dist/bin/tsugumi-pack"
echo "$dist/bin/tsugumi-pack (guest $(cat "$guest/identity"))"
