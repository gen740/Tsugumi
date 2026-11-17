add_library(tsugumi_client STATIC IMPORTED)
set_target_properties(tsugumi_client PROPERTIES
  IMPORTED_LOCATION "${TSUGUMI_CORE_ROOT}/lib/libtsugumi_client.a"
  INTERFACE_INCLUDE_DIRECTORIES "${TSUGUMI_CORE_ROOT}/include;${TSUGUMI_CORE_TEST_SUPPORT}/include")
target_link_libraries(tsugumi_client INTERFACE flatbuffers::flatbuffers CURL::libcurl Threads::Threads)

add_library(tsugumi_client_transport STATIC IMPORTED)
set_target_properties(tsugumi_client_transport PROPERTIES
  IMPORTED_LOCATION "${TSUGUMI_CORE_ROOT}/lib/libtsugumi_client_transport.a")
target_link_libraries(tsugumi_client_transport INTERFACE CURL::libcurl Threads::Threads)
target_link_libraries(tsugumi_client INTERFACE tsugumi_client_transport)
