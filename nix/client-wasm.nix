# Browser RPC transport, cached independently of the Web UI and native core.
{
  lib,
  stdenvNoCC,
  cmake,
  ninja,
  flatbuffers,
  emscripten,
}:
stdenvNoCC.mkDerivation {
  pname = "tsugumi-client-wasm";
  version = lib.fileContents ../VERSION;
  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [
      ../VERSION
      ../CMakeLists.txt
      ../client/CMakeLists.txt
      ../client/include
      ../client/src/client.cc
      ../client/src/client_c.cc
      ../client/src/transport_wasm.cc
      ../client/src/wasm_bindings.cc
      ../protocol/service_common.fbs
      ../protocol/host.fbs
      ../protocol/vm_host.fbs
      ../protocol/agent.fbs
      ../protocol/tsugumi.fbs
      ../protocol/tsugumi_enums.fbs
      ../protocol/data.fbs
      ../protocol/worker.fbs
      ../protocol/remote_worker.fbs
      ../web/scripts/build-client.sh
    ];
  };
  nativeBuildInputs = [
    cmake
    ninja
    flatbuffers
    emscripten
  ];
  dontConfigure = true;
  buildPhase = ''
    runHook preBuild
    export EM_CACHE="$TMPDIR/emscripten-cache"
    sh web/scripts/build-client.sh
    runHook postBuild
  '';
  installPhase = ''
    runHook preInstall
    mkdir -p $out/share/tsugumi/client
    cp build-wasm/client/tsugumi-client.{js,wasm} $out/share/tsugumi/client/
    runHook postInstall
  '';
  meta.description = "Shared Tsugumi RPC client for WebAssembly browsers";
}
