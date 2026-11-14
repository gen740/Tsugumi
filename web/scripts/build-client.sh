#!/bin/sh
# Builds the shared RPC client for browsers using the pinned Emscripten toolchain.
set -eu
if [ -n "${TSUGUMI_WASM_CLIENT_DIR:-}" ]; then
  for artifact in tsugumi-client.js tsugumi-client.wasm; do
    if [ ! -s "$TSUGUMI_WASM_CLIENT_DIR/$artifact" ]; then
      echo "Missing shared RPC client artifact: $TSUGUMI_WASM_CLIENT_DIR/$artifact" >&2
      exit 1
    fi
  done
  exit 0
fi
repo=$(cd "$(dirname "$0")/../.." && pwd)
build="$repo/build-wasm"
flatc_root=$(cd "$(dirname "$(command -v flatc)")/.." && pwd)
emcmake cmake -S "$repo" -B "$build" -G Ninja \
  -DTSUGUMI_BUILD_CORE=OFF -DTSUGUMI_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release \
  -Dflatbuffers_DIR="${flatbuffers_DIR:-$flatc_root/lib/cmake/flatbuffers}"
cmake --build "$build" --target tsugumi_client_wasm
