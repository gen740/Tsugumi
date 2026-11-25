#!/usr/bin/env bash
# Build tsugumi-pack into dist/tsugumi-pack:
#
#   bin/tsugumi-pack                  the exporter and app launcher
#   core/                             Nix-built export-time core
#   Frameworks/libwasmtime.dylib      native Dawn view runtime dependency
#   share/tsugumi-pack/view-licenses/ bundled view runtime licenses
#
# Signing: TSUGUMI_CODESIGN_IDENTITY (for example "Apple Development"), else ad hoc.
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
dist="$repo/dist/tsugumi-pack"
identity="${TSUGUMI_CODESIGN_IDENTITY:--}"
if [ "$(uname -ms)" != "Darwin arm64" ]; then
  echo "tsugumi-pack requires an Apple Silicon Mac" >&2
  exit 1
fi
mkdir -p "$dist/bin" "$dist/share/tsugumi-pack"

nix build -L --accept-flake-config "$repo#tsugumid" --out-link "$dist/core"
for binary in tsugumid tsugumid-pack tsugumi-worker tsugumi-workerd tsugumi-rpc; do
  if [ ! -x "$dist/core/bin/$binary" ]; then
    echo "The native core package is missing bin/$binary" >&2
    exit 1
  fi
done
test -f "$dist/core/share/tsugumi/protocol/tsugumi.bfbs"

# Build the native view host with Apple's compiler and the project's pinned
# Dawn, Wasmtime, and schema generator.
native="$repo/build-pack-host"
sdk=$(/usr/bin/xcrun --sdk macosx --show-sdk-path)
nix develop "$repo" -c bash -eu -c '
  repo=$1
  native=$2
  sdk=$3
  flatbuffers_root=$(cd "$(dirname "$(command -v flatc)")/.." && pwd)
  cmake_path=$(command -v cmake)
  run_apple() { env -i HOME="$HOME" PATH="/usr/bin:/bin:/usr/sbin:/sbin:$PATH" "$@"; }
  run_apple "$cmake_path" -S "$repo" -B "$native" -G Ninja \
    -DTSUGUMI_BUILD_CORE=OFF -DTSUGUMI_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
    -DCMAKE_OSX_SYSROOT="$sdk" -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 \
    -Dflatbuffers_DIR="$flatbuffers_root/lib/cmake/flatbuffers" \
    -DTSUGUMI_FLATC="$(command -v flatc)" -DPython3_EXECUTABLE="$(command -v python3)" \
    -DTSUGUMI_DAWN_DIR="$TSUGUMI_DAWN_DIR" -DTSUGUMI_WEBGPU_HEADERS="$TSUGUMI_WEBGPU_HEADERS" \
    -DTSUGUMI_WASMTIME_DEV="$TSUGUMI_WASMTIME_DEV" -DTSUGUMI_WASMTIME_LIB="$TSUGUMI_WASMTIME_LIB" \
    -DTSUGUMI_VIEW_LIBTOOL=/usr/bin/libtool \
    -DTSUGUMI_CLIENT_CURL_LIBRARY="$sdk/usr/lib/libcurl.tbd"
  run_apple "$cmake_path" --build "$native" --target tsugumi-view-host
  install -d "$native/view-licenses/dawn" "$native/view-licenses/wasmtime"
  install -m 644 "$TSUGUMI_DAWN_DIR/share/licenses/dawn/LICENSE" "$native/view-licenses/dawn/LICENSE"
  install -m 644 "$TSUGUMI_WASMTIME_LICENSE" "$native/view-licenses/wasmtime/LICENSE"
' pack-build "$repo" "$native" "$sdk"

swift_cmd=(env -i HOME="$HOME" PATH="/usr/bin:/bin:/usr/sbin:/sbin" TSUGUMI_CLIENT_LIB_DIR="$native/client" swift build -c release --scratch-path "$native/swift" --package-path "$repo/pack/launcher")
"${swift_cmd[@]}" --product tsugumi-pack
cp "$("${swift_cmd[@]}" --show-bin-path)/tsugumi-pack" "$dist/bin/tsugumi-pack"
mkdir -p "$dist/Frameworks"
install -d "$dist/share/tsugumi-pack/view-licenses"
cp -R "$native/view-licenses/." "$dist/share/tsugumi-pack/view-licenses/"
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
codesign --force --sign "$identity" --timestamp=none "$wasmtime"
entitlements="$(mktemp -t tsugumi-pack)"
trap 'rm -f "$entitlements"' EXIT
cat >"$entitlements" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>com.apple.security.cs.allow-jit</key><true/></dict></plist>
EOF
options=(--timestamp=none)
[ "$identity" = - ] || options+=(--options runtime)
codesign --force --sign "$identity" ${options[@]+"${options[@]}"} --entitlements "$entitlements" "$dist/bin/tsugumi-pack"
echo "$dist/bin/tsugumi-pack"
