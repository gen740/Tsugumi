add_library(tsugumi_json STATIC IMPORTED)
set_target_properties(tsugumi_json PROPERTIES
  IMPORTED_LOCATION "${TSUGUMI_CORE_TEST_SUPPORT}/lib/libtsugumi_json.a")
target_link_libraries(tsugumi_json INTERFACE hosq)
