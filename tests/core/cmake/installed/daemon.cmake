add_library(tsugumi_core STATIC IMPORTED)
set_target_properties(tsugumi_core PROPERTIES
  IMPORTED_LOCATION "${TSUGUMI_CORE_TEST_SUPPORT}/lib/libtsugumi_core.a"
  INTERFACE_INCLUDE_DIRECTORIES "${TSUGUMI_CORE_TEST_SUPPORT}/include")
# As in-tree, users choose either tsugumi_nix_eval or the evaluator double.
target_link_libraries(tsugumi_core INTERFACE tsugumi_trace_codec tsugumi_rpc_schema tsugumi_transport tsugumi_worker_values tsugumi_runtime_support tsugumi_nix_store tsugumi_launch_paths tsugumi_document tsugumi_workerd_support tsugumi_json flatbuffers::flatbuffers
  PkgConfig::SQLITE3 PkgConfig::LIBGIT2 Threads::Threads)
if(NOT APPLE)
  pkg_check_modules(CRYPTO REQUIRED IMPORTED_TARGET libcrypto)
  target_link_libraries(tsugumi_workerd_support INTERFACE PkgConfig::CRYPTO)
  target_link_libraries(tsugumi_core INTERFACE PkgConfig::CRYPTO)
endif()
