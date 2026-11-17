# CI builds only its external dependency double and public RPC scenarios
# against these existing archives. The normal runtime contains no test code.
set(TSUGUMI_TEST_SUPPORT_DIR "" CACHE PATH "Install reusable core test build inputs")
if(TSUGUMI_TEST_SUPPORT_DIR)
  install(TARGETS tsugumi_worker_authorization tsugumi_runtime_support tsugumi_nix_store tsugumi_transport tsugumi_worker_values tsugumi_util tsugumi_core tsugumi_document tsugumi_nix_eval tsugumi_host_core tsugumi_sdk_describe tsugumi_json tsugumi_workerd_support tsugumi_worker_protocol tsugumi_peer_tls
    ARCHIVE DESTINATION ${TSUGUMI_TEST_SUPPORT_DIR}/lib)
  install(FILES ${PROJECT_SOURCE_DIR}/sdk/describe/input.hh DESTINATION ${TSUGUMI_TEST_SUPPORT_DIR}/include/tsugumi-sdk-describe)
  install(DIRECTORY ${PROJECT_BINARY_DIR}/headers/ DESTINATION ${TSUGUMI_TEST_SUPPORT_DIR}/include/tsugumi-owned)
  install(DIRECTORY include/ DESTINATION ${TSUGUMI_TEST_SUPPORT_DIR}/include)
  install(DIRECTORY ${PROJECT_SOURCE_DIR}/workerd/include/ DESTINATION ${TSUGUMI_TEST_SUPPORT_DIR}/include)
  install(FILES ${PROJECT_SOURCE_DIR}/workerd/src/journal.hh DESTINATION ${TSUGUMI_TEST_SUPPORT_DIR}/include/tsugumi-workerd)
  install(DIRECTORY src/ DESTINATION ${TSUGUMI_TEST_SUPPORT_DIR}/include/tsugumi-core
    FILES_MATCHING PATTERN "*.hh")
  install(FILES src/main.cc src/project_endpoint.cc ${PROJECT_SOURCE_DIR}/workerd/src/local_service.cc DESTINATION ${TSUGUMI_TEST_SUPPORT_DIR}/src)
  install(DIRECTORY ${TSUGUMI_GEN_DIR}/cpp/ DESTINATION ${TSUGUMI_TEST_SUPPORT_DIR}/include)
  install(DIRECTORY ${TSUGUMI_GEN_DIR}/python/ DESTINATION ${TSUGUMI_TEST_SUPPORT_DIR}/python)
endif()
