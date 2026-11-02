#!/usr/bin/env bash
# Builds libtsugumi_renderer.dylib (Dawn pinned in CMakeLists.txt) with the
# Apple toolchain; cmake/ninja/flatc come from `nix develop`.
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
build="${TSUGUMI_RENDERER_BUILD:-$repo/build-renderer}"
env -u CC -u CXX -u SDKROOT -u NIX_CFLAGS_COMPILE -u NIX_LDFLAGS -u MACOSX_DEPLOYMENT_TARGET \
  cmake -S "$repo" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -DCMAKE_OBJCXX_COMPILER=/usr/bin/clang++ \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 \
    -DTSUGUMI_BUILD_CORE=OFF -DTSUGUMI_BUILD_TESTS=OFF -DTSUGUMI_BUILD_RENDERER=ON
env -u CC -u CXX -u SDKROOT -u NIX_CFLAGS_COMPILE -u NIX_LDFLAGS cmake --build "$build" --target tsugumi_renderer
echo "$build/ui/renderer/libtsugumi_renderer.dylib"
