# Reflection bytes are wire resources shared by the Agent catalog and tracing.
function(embed_rpc_schema schema symbol sources)
  set(output ${CMAKE_CURRENT_BINARY_DIR}/${symbol}.cc)
  add_custom_command(
    OUTPUT ${output}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${TSUGUMI_GEN_DIR}/bfbs/${schema}.bfbs
      -DOUTPUT=${output} -DSYMBOL=${symbol}
      -P ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS ${TSUGUMI_GEN_DIR}/bfbs/${schema}.bfbs ${PROJECT_SOURCE_DIR}/cmake/embed.cmake VERBATIM)
  set(${sources} ${${sources}} ${output} PARENT_SCOPE)
endfunction()

embed_rpc_schema(tsugumi tsugumi_rpc_bfbs schema_sources)
embed_rpc_schema(host tsugumi_host_bfbs schema_sources)
foreach(schema agent worker local_worker remote_worker vm_host)
  embed_rpc_schema(${schema} tsugumi_${schema}_bfbs schema_sources)
endforeach()
add_library(tsugumi_rpc_schema STATIC ${schema_sources})
add_library(tsugumi_trace_codec STATIC trace_codec.cc)
target_include_directories(tsugumi_trace_codec PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tsugumi_trace_codec PUBLIC tsugumi_protocol tsugumi_trace_store
  PRIVATE tsugumi_rpc_schema flatbuffers::flatbuffers)
install(DIRECTORY include/ DESTINATION include)
install(FILES artifact.fbs DESTINATION include/tsugumi)
