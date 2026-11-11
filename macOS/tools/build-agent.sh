#!/usr/bin/env bash
# Native Agent/client use Apple's compiler, SDK, TLS trust and SQLite.
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
    HOSQ_INCLUDE_DIR="$HOSQ_INCLUDE_DIR" PKG_CONFIG_PATH="${PKG_CONFIG_PATH:-}" \
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
run "$cmake_path" --build "$repo/build-host" --target tsugumi-agentd tsugumi-rpc

for binary in "$repo/build-host/core/tsugumi-agentd" "$repo/build-host/client/tsugumi-rpc"; do
  if /usr/bin/otool -L "$binary" | /usr/bin/grep -q '/nix/store/'; then
    echo "The distributable binary still depends on the Nix store: $binary" >&2
    exit 1
  fi
done
