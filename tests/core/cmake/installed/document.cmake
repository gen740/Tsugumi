add_library(tsugumi_document STATIC IMPORTED)
set_target_properties(tsugumi_document PROPERTIES
  IMPORTED_LOCATION "${TSUGUMI_CORE_TEST_SUPPORT}/lib/libtsugumi_document.a"
  INTERFACE_INCLUDE_DIRECTORIES "${TSUGUMI_CORE_TEST_SUPPORT}/include")
target_link_libraries(tsugumi_document INTERFACE tsugumi_json flatbuffers::flatbuffers
  Threads::Threads tsugumi_util)
