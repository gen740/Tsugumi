add_library(tsugumi_host_core STATIC IMPORTED)
set_target_properties(tsugumi_host_core PROPERTIES
  IMPORTED_LOCATION "${TSUGUMI_CORE_TEST_SUPPORT}/lib/libtsugumi_host_core.a"
  INTERFACE_INCLUDE_DIRECTORIES "${TSUGUMI_CORE_TEST_SUPPORT}/include;${TSUGUMI_CORE_TEST_SUPPORT}/include")
target_link_libraries(tsugumi_host_core INTERFACE tsugumi_trace_codec tsugumi_rpc_schema tsugumi_transport tsugumi_sdk_describe tsugumi_json flatbuffers::flatbuffers
  Threads::Threads tsugumi_document tsugumi_client PkgConfig::LIBGIT2 CURL::libcurl tsugumi_worker_protocol tsugumi_peer_tls tsugumi_launch_paths PkgConfig::SQLITE3)
if(APPLE)
  target_link_libraries(tsugumi_host_core INTERFACE "-framework CoreFoundation")
else()
  find_package(ICU REQUIRED COMPONENTS uc i18n)
  target_link_libraries(tsugumi_host_core INTERFACE ICU::uc ICU::i18n PkgConfig::CRYPTO)
endif()
