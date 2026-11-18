add_library(tsugumi_json STATIC IMPORTED)
set_target_properties(tsugumi_json PROPERTIES
  IMPORTED_LOCATION "${TSUGUMI_CORE_TEST_SUPPORT}/lib/libtsugumi_json.a"
  INTERFACE_INCLUDE_DIRECTORIES "${TSUGUMI_CORE_TEST_SUPPORT}/include")
target_link_libraries(tsugumi_json INTERFACE hosq tsugumi_util)

add_library(tsugumi_util STATIC IMPORTED)
set_target_properties(tsugumi_util PROPERTIES
  IMPORTED_LOCATION "${TSUGUMI_CORE_TEST_SUPPORT}/lib/libtsugumi_util.a"
  INTERFACE_INCLUDE_DIRECTORIES "${TSUGUMI_CORE_TEST_SUPPORT}/include")
target_link_libraries(tsugumi_util INTERFACE tsugumi_crypto PkgConfig::SQLITE3 Threads::Threads)
if(NOT APPLE)
  pkg_check_modules(CRYPTO REQUIRED IMPORTED_TARGET libcrypto)
  target_link_libraries(tsugumi_util INTERFACE PkgConfig::CRYPTO)
endif()
add_library(tsugumi_transport STATIC IMPORTED)
set_target_properties(tsugumi_transport PROPERTIES
  IMPORTED_LOCATION "${TSUGUMI_CORE_TEST_SUPPORT}/lib/libtsugumi_transport.a"
  INTERFACE_INCLUDE_DIRECTORIES "${TSUGUMI_CORE_TEST_SUPPORT}/include")
target_link_libraries(tsugumi_transport INTERFACE tsugumi_util tsugumi_json Threads::Threads)
add_library(tsugumi_runtime_support STATIC IMPORTED)
set_target_properties(tsugumi_runtime_support PROPERTIES
  IMPORTED_LOCATION "${TSUGUMI_CORE_TEST_SUPPORT}/lib/libtsugumi_runtime_support.a"
  INTERFACE_INCLUDE_DIRECTORIES "${TSUGUMI_CORE_TEST_SUPPORT}/include;${TSUGUMI_CORE_TEST_SUPPORT}/include")
target_link_libraries(tsugumi_runtime_support INTERFACE tsugumi_util flatbuffers::flatbuffers)

add_library(tsugumi_rpc_schema STATIC IMPORTED)
set_target_properties(tsugumi_rpc_schema PROPERTIES IMPORTED_LOCATION "${TSUGUMI_CORE_TEST_SUPPORT}/lib/libtsugumi_rpc_schema.a")
