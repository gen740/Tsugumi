add_library(tsugumi_peer_tls STATIC IMPORTED)
set_target_properties(tsugumi_peer_tls PROPERTIES
  IMPORTED_LOCATION "${TSUGUMI_CORE_TEST_SUPPORT}/lib/libtsugumi_peer_tls.a"
  INTERFACE_INCLUDE_DIRECTORIES "${TSUGUMI_CORE_TEST_SUPPORT}/include")
target_link_libraries(tsugumi_peer_tls INTERFACE OpenSSL::SSL tsugumi_util)

add_library(tsugumi_worker_authorization STATIC IMPORTED)
set_target_properties(tsugumi_worker_authorization PROPERTIES
  IMPORTED_LOCATION "${TSUGUMI_CORE_TEST_SUPPORT}/lib/libtsugumi_worker_authorization.a"
  INTERFACE_INCLUDE_DIRECTORIES "${TSUGUMI_CORE_TEST_SUPPORT}/include")
target_link_libraries(tsugumi_worker_authorization INTERFACE tsugumi_peer_tls)
