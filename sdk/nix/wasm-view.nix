# One Emscripten main module for every Card; each Card supplies only a side module.
{ pkgs }:
let
  inherit (pkgs) lib;
  source = ../views/wasm;
  # This is the remote port pinned by Emscripten 6.0.9 in our nixpkgs. Fetch it
  # through Nix, never through the compiler's network-enabled port downloader.
  portArchive = pkgs.fetchurl {
    url = "https://github.com/google/dawn/releases/download/v20260423.175430/emdawnwebgpu_pkg-v20260423.175430.zip";
    hash = "sha512-QnhPcLZxl8YUMi+fq7Dx3GQiig3hDoj5mUH6nSm+6a1mg/RlHU7v1aep+9H5dutSKxkLaDIZ7ReT6bUxxgL/pg==";
  };
  port = pkgs.runCommand "tsugumi-emdawnwebgpu" { nativeBuildInputs = [ pkgs.unzip ]; } ''
    unzip -q ${portArchive}
    mv emdawnwebgpu_pkg $out
  '';
  # Use the standard webgpu-headers snapshot already pinned by nixpkgs, without
  # building wgpu-native's Rust library. The port's own extensions stay private.
  headers = pkgs.runCommand "tsugumi-view-headers" { } ''
    mkdir -p $out/include/webgpu $out/include/tsugumi $out/share/licenses
    cp ${pkgs.wgpu-native.src}/ffi/webgpu-headers/webgpu.h $out/include/webgpu/
    cp ${../include/tsugumi/view.h} $out/include/tsugumi/view.h
    cp ${pkgs.wgpu-native.src}/ffi/webgpu-headers/LICENSE $out/share/licenses/webgpu-headers
  '';
  template =
    pkgs.runCommand "tsugumi-wasm-view-template"
      {
        nativeBuildInputs = [
          pkgs.emscripten
          pkgs.esbuild
        ];
      }
      ''
        export EM_CACHE="$TMPDIR/emscripten-cache"
        mkdir -p $out
        # MAIN_MODULE=1 deliberately retains libc/C++ and the full WebGPU binding:
        # a new Card must never require a differently tree-shaken shared template.
        em++ ${source}/runtime.cc -std=c++20 -O2 -fPIC -Wall -Wextra -Werror \
          -I${headers}/include -c -o runtime.o
        em++ runtime.o -O2 --no-entry \
          --use-port=${port}/emdawnwebgpu.port.py:cpp_bindings=false \
          --js-library=${source}/library.js \
          -sMAIN_MODULE=1 -sALLOW_MEMORY_GROWTH=1 \
          -sMODULARIZE=1 -sEXPORT_ES6=1 -sSINGLE_FILE=1 -sENVIRONMENT=web \
          -sEXPORTED_FUNCTIONS='["_tsugumi_host_pointer","_tsugumi_host_wheel","_tsugumi_host_frame","_tsugumi_host_format","_tsugumi_host_configure","_tsugumi_host_device","_tsugumi_host_surface","_wgpuSurfaceRelease","_wgpuDeviceRelease","_malloc","_free"]' \
          -sEXPORTED_RUNTIME_METHODS='["loadWebAssemblyModule","HEAPU8"]' \
          -o runtime.js
        cp ${source}/index.js index.js
        esbuild index.js --bundle --format=esm --platform=browser --target=es2022 \
          --minify --outfile=$out/index.js
        # Keep upstream license notices alongside the shared implementation.
        cp ${port}/webgpu/src/LICENSE $out/LICENSE.emdawnwebgpu
        cp ${pkgs.emscripten}/share/emscripten/LICENSE $out/LICENSE.emscripten
        wc -c < $out/index.js > $out/size-bytes
      '';
in
{
  inherit headers template;
  build =
    {
      src,
      sources ? [ "main.c" ],
      name ? "tsugumi-wasm-view",
      cflags ? [ ],
    }:
    pkgs.runCommand name { nativeBuildInputs = [ pkgs.emscripten ]; } ''
      export EM_CACHE="$TMPDIR/emscripten-cache"
      mkdir -p $out
      objects=()
      for source in ${lib.escapeShellArgs sources}; do
        case "$source" in
          *.c) compiler=emcc; standard=c11 ;;
          *) compiler=em++; standard=c++20 ;;
        esac
        object="view-''${#objects[@]}.o"
        "$compiler" -std="$standard" -O2 -fPIC -Wall -Wextra -Werror \
          -I${headers}/include ${lib.escapeShellArgs cflags} -c ${src}/"$source" -o "$object"
        objects+=("$object")
      done
      em++ "''${objects[@]}" -O2 -sSIDE_MODULE=2 \
        -sEXPORTED_FUNCTIONS='["_tsugumi_view_initialise","_tsugumi_view_update","_tsugumi_view_resize","_tsugumi_view_dispose"]' \
        -o $out/view.wasm
      ln -s ${template}/index.js $out/index.js
      wc -c < $out/view.wasm > $out/size-bytes
    '';
}
