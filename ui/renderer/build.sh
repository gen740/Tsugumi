#!/usr/bin/env bash
# Builds libtsugumi_renderer.dylib (Dawn pinned in CMakeLists.txt) with the
# Apple toolchain. Only the cmake, ninja, flatc and python3 binaries are taken
# from Nix; no Nix compiler or SDK environment is used.
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
build="${TSUGUMI_RENDERER_BUILD:-$repo/build-renderer}"
tools=""
for p in cmake ninja flatbuffers python3; do
  tools="$tools:$(nix build --no-link --print-out-paths --inputs-from "$repo" "nixpkgs#$p")/bin"
done
sdk="$(/usr/bin/xcrun --sdk macosx --show-sdk-path)"
run() {
  env -i HOME="$HOME" PATH="/usr/bin:/bin:/usr/sbin:/sbin${tools}" "$@"
}
run cmake -S "$repo" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -DCMAKE_OBJCXX_COMPILER=/usr/bin/clang++ \
  -DCMAKE_OSX_SYSROOT="$sdk" -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 \
  -DTSUGUMI_BUILD_CORE=OFF -DTSUGUMI_BUILD_TESTS=OFF -DTSUGUMI_BUILD_RENDERER=ON \
  -DHOSQ_INCLUDE_DIR="$(nix eval --raw --impure --expr "(builtins.getFlake \"$repo\").inputs.hosq.outPath")/include"
run cmake --build "$build" --target tsugumi_renderer
echo "$build/ui/renderer/libtsugumi_renderer.dylib"
