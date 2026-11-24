#!/usr/bin/env bash
# Native host/client use Apple's compiler, SDK, provider HTTPS trust and SQLite.
# Remote-worker TLS uses the OpenSSL already supplied by the development shell.
# Nix supplies pinned build tools, FlatBuffers and source-only dependencies.
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
if [ "${TSUGUMI_HOST_BUILD_ENV:-}" != 1 ]; then
  exec nix develop "$repo" -c env TSUGUMI_HOST_BUILD_ENV=1 "$0"
fi
sdk_path=$(env -i PATH=/usr/bin:/bin /usr/bin/xcrun --sdk macosx --show-sdk-path)
flatbuffers_root=$(cd "$(dirname "$(command -v flatc)")/.." && pwd)
cmake_path=$(command -v cmake)
run() {
  env -i HOME="$HOME" PATH="/usr/bin:/bin:/usr/sbin:/sbin:$PATH" \
    TSUGUMI_FLATBUFFERS_SOURCE="${TSUGUMI_FLATBUFFERS_SOURCE:?}" \
    HOSQ_INCLUDE_DIR="$HOSQ_INCLUDE_DIR" \
    TSUGUMI_CACERT="${TSUGUMI_CACERT:?}" \
    TSUGUMI_CARD_NIXPKGS_REV="${TSUGUMI_CARD_NIXPKGS_REV:?}" \
    TSUGUMI_CARD_NIXPKGS_NARHASH="${TSUGUMI_CARD_NIXPKGS_NARHASH:?}" \
    TSUGUMI_DAWN_DIR="${TSUGUMI_DAWN_DIR:?}" \
    TSUGUMI_WASMTIME_DEV="${TSUGUMI_WASMTIME_DEV:?}" \
    TSUGUMI_WASMTIME_LIB="${TSUGUMI_WASMTIME_LIB:?}" \
    TSUGUMI_WEBGPU_HEADERS="${TSUGUMI_WEBGPU_HEADERS:?}" \
    PKG_CONFIG_PATH="${PKG_CONFIG_PATH:-}" \
    CMAKE_PREFIX_PATH="${CMAKE_PREFIX_PATH:-}" "$@"
}
run "$cmake_path" --fresh -S "$repo" -B "$repo/build-host" -G Ninja \
  -DTSUGUMI_FORMAT_NIX="@bundle/nixfmt" -DTSUGUMI_TEMPLATE_GIT="@bundle/git" \
  -DTSUGUMI_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release \
  -DPython3_EXECUTABLE="$(command -v python3)" \
  -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -DCMAKE_OSX_SYSROOT="$sdk_path" \
  -Dflatbuffers_DIR="$flatbuffers_root/lib/cmake/flatbuffers" \
  -DTSUGUMI_CLIENT_CURL_LIBRARY="$sdk_path/usr/lib/libcurl.tbd" \
  -DCURL_LIBRARY="$sdk_path/usr/lib/libcurl.tbd" -DCURL_LIBRARY_RELEASE="$sdk_path/usr/lib/libcurl.tbd" \
  -DCURL_LIBRARY_DEBUG="$sdk_path/usr/lib/libcurl.tbd" -DCURL_INCLUDE_DIR="$sdk_path/usr/include" \
  -DTSUGUMI_HOST_SQLITE_LIBRARY="$sdk_path/usr/lib/libsqlite3.tbd"
run "$cmake_path" --build "$repo/build-host" --target tsugumi-hostd tsugumi tsugumi-view-host

view_licenses="$repo/build-host/Resources/view-licenses"
install -d "$view_licenses/dawn" "$view_licenses/wasmtime"
install -m 644 "$TSUGUMI_DAWN_DIR/share/licenses/dawn/LICENSE" "$view_licenses/dawn/LICENSE"
install -m 644 "${TSUGUMI_WASMTIME_LICENSE:?}" "$view_licenses/wasmtime/LICENSE"

# Copy pinned nixfmt and its dylibs for use outside the Nix store.
# Apple's tools from Xcode: the shell's older SDK strip rejects these dylibs.
env -u DEVELOPER_DIR -u SDKROOT python3 "$repo/ui/macos/tools/bundle-formatters.py" "$repo/build-host/Resources/formatters" \
  "${TSUGUMI_FORMAT_NIX:?}"

# Build the two commands used by template import against Apple's system libs.
# The /usr/bin/git shim depends on developer tools on the user's Mac.
git_build="$(mktemp -d "$repo/build-host/git-build.XXXXXX")"
trap 'rm -rf -- "$git_build"' EXIT
tar -xf "${TSUGUMI_TEMPLATE_GIT_SOURCE:?}" -C "$git_build" --strip-components 1
env -i PATH=/usr/bin:/bin HOME="$HOME" SDKROOT="$sdk_path" MACOSX_DEPLOYMENT_TARGET=26.0 \
  /usr/bin/make -C "$git_build" -j "${CMAKE_BUILD_PARALLEL_LEVEL:-8}" CC=/usr/bin/clang \
    NO_GETTEXT=YesPlease NO_OPENSSL=YesPlease NO_PERL=YesPlease NO_PYTHON=YesPlease \
    NO_TCLTK=YesPlease NO_RUST=YesPlease git git-remote-http
git_tools="$repo/build-host/Resources/git"
git_licenses="$repo/build-host/Resources/git-licenses"
rm -rf -- "$git_tools" "$git_licenses"
install -d "$git_tools" "$git_licenses"
install -m 755 "$git_build/git" "$git_tools/git"
install -m 755 "$git_build/git-remote-http" "$git_tools/git-remote-https"
for tool in "$git_tools/git" "$git_tools/git-remote-https"; do
  /usr/bin/strip -S "$tool"
  /usr/bin/codesign --force --sign - "$tool"
done
install -m 644 "$git_build/COPYING" "$git_licenses/COPYING"
printf '%s\n' "${TSUGUMI_TEMPLATE_GIT_SOURCE_URL:?}" > "$git_licenses/SOURCE"

# Bundle the two OpenSSL runtime libraries, keeping the native product usable
# without a Nix installation. Provider HTTPS still uses the system libcurl.
frameworks="$repo/build-host/Frameworks"
mkdir -p "$frameworks"
ssl_library=$(sed -n 's/^OPENSSL_SSL_LIBRARY:FILEPATH=//p' "$repo/build-host/CMakeCache.txt")
crypto_library=$(sed -n 's/^OPENSSL_CRYPTO_LIBRARY:FILEPATH=//p' "$repo/build-host/CMakeCache.txt")
ssl_id=$(/usr/bin/otool -D "$ssl_library" | tail -n 1)
crypto_id=$(/usr/bin/otool -D "$crypto_library" | tail -n 1)
cp -L "$ssl_library" "$frameworks/libssl.3.dylib"
cp -L "$crypto_library" "$frameworks/libcrypto.3.dylib"
chmod u+w "$frameworks/"*.dylib
/usr/bin/install_name_tool -id @rpath/libssl.3.dylib -change "$crypto_id" @loader_path/libcrypto.3.dylib "$frameworks/libssl.3.dylib"
/usr/bin/install_name_tool -id @rpath/libcrypto.3.dylib "$frameworks/libcrypto.3.dylib"
for executable in "$repo/build-host/hostd/tsugumi-hostd" "$repo/build-host/cli/tsugumi"; do
  /usr/bin/install_name_tool -change "$ssl_id" @executable_path/../Frameworks/libssl.3.dylib \
    -change "$crypto_id" @executable_path/../Frameworks/libcrypto.3.dylib "$executable"
done
# install_name_tool invalidates the linker-created ad-hoc signature. The app
# build signs these again with the selected development identity.
for binary in "$frameworks/"*.dylib "$repo/build-host/hostd/tsugumi-hostd" "$repo/build-host/cli/tsugumi"; do
  /usr/bin/codesign --force --sign - "$binary"
done

for binary in "$repo/build-host/hostd/tsugumi-hostd" "$repo/build-host/cli/tsugumi" "$frameworks/"*.dylib; do
  if /usr/bin/otool -L "$binary" | /usr/bin/grep -q '/nix/store/'; then
    echo "The distributable binary still depends on the Nix store: $binary" >&2
    exit 1
  fi
done
