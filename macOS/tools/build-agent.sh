#!/usr/bin/env bash
# Native Agent/client use Apple's compiler, SDK, provider HTTPS trust and SQLite.
# Remote-worker TLS uses the OpenSSL already supplied by the development shell.
# Nix supplies pinned build tools, FlatBuffers and source-only dependencies.
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
if [ "${TSUGUMI_AGENT_BUILD_ENV:-}" != 1 ]; then
  exec nix develop "$repo" -c env TSUGUMI_AGENT_BUILD_ENV=1 "$0"
fi
sdk_path=$(env -i PATH=/usr/bin:/bin /usr/bin/xcrun --sdk macosx --show-sdk-path)
flatbuffers_root=$(cd "$(dirname "$(command -v flatc)")/.." && pwd)
cmake_path=$(command -v cmake)
run() {
  env -i HOME="$HOME" PATH="/usr/bin:/bin:/usr/sbin:/sbin:$PATH" \
    HOSQ_INCLUDE_DIR="$HOSQ_INCLUDE_DIR" \
    TSUGUMI_CARD_NIXPKGS_REV="${TSUGUMI_CARD_NIXPKGS_REV:?}" \
    TSUGUMI_CARD_NIXPKGS_NARHASH="${TSUGUMI_CARD_NIXPKGS_NARHASH:?}" \
    PKG_CONFIG_PATH="${PKG_CONFIG_PATH:-}" \
    CMAKE_PREFIX_PATH="${CMAKE_PREFIX_PATH:-}" "$@"
}
run "$cmake_path" --fresh -S "$repo" -B "$repo/build-host" -G Ninja \
  -DTSUGUMI_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -DCMAKE_OSX_SYSROOT="$sdk_path" \
  -Dflatbuffers_DIR="$flatbuffers_root/lib/cmake/flatbuffers" \
  -DTSUGUMI_CLIENT_CURL_LIBRARY="$sdk_path/usr/lib/libcurl.tbd" \
  -DCURL_LIBRARY="$sdk_path/usr/lib/libcurl.tbd" -DCURL_LIBRARY_RELEASE="$sdk_path/usr/lib/libcurl.tbd" \
  -DCURL_LIBRARY_DEBUG="$sdk_path/usr/lib/libcurl.tbd" -DCURL_INCLUDE_DIR="$sdk_path/usr/include" \
  -DTSUGUMI_AGENT_SQLITE_LIBRARY="$sdk_path/usr/lib/libsqlite3.tbd"
run "$cmake_path" --build "$repo/build-host" --target tsugumi-agentd

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
/usr/bin/install_name_tool -change "$ssl_id" @executable_path/../Frameworks/libssl.3.dylib \
  -change "$crypto_id" @executable_path/../Frameworks/libcrypto.3.dylib "$repo/build-host/core/tsugumi-agentd"
# install_name_tool invalidates the linker-created ad-hoc signature. The app
# build signs these again with the selected development identity.
for binary in "$frameworks/"*.dylib "$repo/build-host/core/tsugumi-agentd"; do
  /usr/bin/codesign --force --sign - "$binary"
done

for binary in "$repo/build-host/core/tsugumi-agentd" "$frameworks/"*.dylib; do
  if /usr/bin/otool -L "$binary" | /usr/bin/grep -q '/nix/store/'; then
    echo "The distributable binary still depends on the Nix store: $binary" >&2
    exit 1
  fi
done
