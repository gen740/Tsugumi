add_library(tsugumi_worker_protocol STATIC IMPORTED)
set_target_properties(tsugumi_worker_protocol PROPERTIES
  IMPORTED_LOCATION "${TSUGUMI_CORE_TEST_SUPPORT}/lib/libtsugumi_worker_protocol.a"
  INTERFACE_INCLUDE_DIRECTORIES "${TSUGUMI_CORE_TEST_SUPPORT}/include")
target_link_libraries(tsugumi_worker_protocol INTERFACE flatbuffers::flatbuffers)
add_library(tsugumi_workerd_support STATIC IMPORTED)
set_target_properties(tsugumi_workerd_support PROPERTIES
  IMPORTED_LOCATION "${TSUGUMI_CORE_TEST_SUPPORT}/lib/libtsugumi_workerd_support.a"
  INTERFACE_INCLUDE_DIRECTORIES "${TSUGUMI_CORE_TEST_SUPPORT}/include")
target_link_libraries(tsugumi_workerd_support INTERFACE tsugumi_worker_protocol tsugumi_json tsugumi_util)
set(_local_service_source ${TSUGUMI_CORE_TEST_SUPPORT}/src/local_service.cc)
set(_workerd_headers ${TSUGUMI_CORE_TEST_SUPPORT}/include/tsugumi-workerd)

add_library(tsugumi_worker_values STATIC IMPORTED)
set_target_properties(tsugumi_worker_values PROPERTIES
  IMPORTED_LOCATION "${TSUGUMI_CORE_TEST_SUPPORT}/lib/libtsugumi_worker_values.a"
  INTERFACE_INCLUDE_DIRECTORIES "${TSUGUMI_CORE_TEST_SUPPORT}/include")
target_link_libraries(tsugumi_worker_values INTERFACE tsugumi_document tsugumi_util)
