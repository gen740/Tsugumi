target_sources(tsugumi_client PRIVATE src/transport_wasm.cc)
target_compile_options(tsugumi_client PRIVATE -fexceptions)
target_link_options(tsugumi_client PUBLIC -fexceptions -sDISABLE_EXCEPTION_CATCHING=0 -lwebsocket.js)
