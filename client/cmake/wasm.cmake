target_compile_options(tsugumi_client PRIVATE -fexceptions)
target_link_libraries(tsugumi_client PUBLIC tsugumi_client_transport)

add_executable(tsugumi_client_wasm src/wasm_bindings.cc)
target_link_libraries(tsugumi_client_wasm PRIVATE tsugumi_client)
target_link_options(tsugumi_client_wasm PRIVATE --bind --no-entry -sMODULARIZE=1 -sEXPORT_ES6=1
  -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=web,worker -sNO_EXIT_RUNTIME=1
  # hostd's CSP has no 'unsafe-eval': embind must not build invokers with new Function.
  -sDYNAMIC_EXECUTION=0)
set_target_properties(tsugumi_client_wasm PROPERTIES OUTPUT_NAME tsugumi-client SUFFIX .js)
