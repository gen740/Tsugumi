add_library(tsugumi_sdk_describe STATIC IMPORTED)
set_target_properties(tsugumi_sdk_describe PROPERTIES
  IMPORTED_LOCATION "${TSUGUMI_CORE_TEST_SUPPORT}/lib/libtsugumi_sdk_describe.a"
  INTERFACE_INCLUDE_DIRECTORIES "${TSUGUMI_CORE_TEST_SUPPORT}/include/tsugumi-sdk-describe")
target_link_libraries(tsugumi_sdk_describe INTERFACE flatbuffers::flatbuffers)
