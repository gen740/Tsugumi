# Reflection bytes are a wire resource, independent of either endpoint.
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/tsugumi_rpc_bfbs.cc
  COMMAND ${CMAKE_COMMAND} -DINPUT=${TSUGUMI_GEN_DIR}/bfbs/tsugumi.bfbs
    -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/tsugumi_rpc_bfbs.cc -DSYMBOL=tsugumi_rpc_bfbs
    -P ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
  DEPENDS ${TSUGUMI_GEN_DIR}/bfbs/tsugumi.bfbs ${PROJECT_SOURCE_DIR}/cmake/embed.cmake VERBATIM)
add_library(tsugumi_rpc_schema STATIC ${CMAKE_CURRENT_BINARY_DIR}/tsugumi_rpc_bfbs.cc)
