add_library(tsugumi_peer_tls STATIC IMPORTED)
set_target_properties(tsugumi_peer_tls PROPERTIES
  IMPORTED_LOCATION "${TSUGUMI_CORE_TEST_SUPPORT}/lib/libtsugumi_peer_tls.a"
  INTERFACE_INCLUDE_DIRECTORIES "${TSUGUMI_CORE_TEST_SUPPORT}/include")
target_link_libraries(tsugumi_peer_tls INTERFACE OpenSSL::SSL tsugumi_util)

