add_executable(tsugumi_rpc_cli_tests rpc_cli_test.cc agent_fixture_server.cc)
target_link_libraries(tsugumi_rpc_cli_tests PRIVATE tsugumi_client tsugumi_document tsugumi_json GTest::gtest_main hosq Threads::Threads)
if(TSUGUMI_INSTALLED_CORE)
  target_include_directories(tsugumi_rpc_cli_tests PRIVATE ${TSUGUMI_CORE_ROOT}/include)
else()
  target_include_directories(tsugumi_rpc_cli_tests PRIVATE ${PROJECT_SOURCE_DIR}/rpc/include)
endif()
target_compile_options(tsugumi_rpc_cli_tests PRIVATE -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough)
target_compile_definitions(tsugumi_rpc_cli_tests PRIVATE
  TSUGUMI_RPC_BINARY="$<TARGET_FILE:tsugumi-rpc>"
  TSUGUMI_PROJECT_BINARY="${_test_mock}"
  TSUGUMI_WORKER_BINARY="$<TARGET_FILE:tsugumi-worker>"
  TSUGUMI_PROJECT_SCHEMA="${_test_project_schema}"
  TSUGUMI_TEST_NIX="${_test_nix}")
add_dependencies(tsugumi_rpc_cli_tests tsugumi-rpc tsugumid-mock)
gtest_discover_tests(tsugumi_rpc_cli_tests TEST_PREFIX "core_rpc_cli." DISCOVERY_MODE PRE_TEST
  PROPERTIES TIMEOUT 30)

add_executable(tsugumi_rpc_trace_tests rpc_trace_test.cc)
target_link_libraries(tsugumi_rpc_trace_tests PRIVATE tsugumi_trace_codec
  flatbuffers::flatbuffers GTest::gtest_main)
gtest_discover_tests(tsugumi_rpc_trace_tests TEST_PREFIX "core_rpc_trace."
  DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 30 ENVIRONMENT "TSUGUMI_RPC_TRACE=1")
add_executable(tsugumi_rpc_frame_tests rpc_frame_test.cc)
target_link_libraries(tsugumi_rpc_frame_tests PRIVATE tsugumi_client_transport GTest::gtest_main)
if(TSUGUMI_INSTALLED_CORE)
  target_include_directories(tsugumi_rpc_frame_tests PRIVATE ${TSUGUMI_CORE_ROOT}/include)
else()
  target_include_directories(tsugumi_rpc_frame_tests PRIVATE ${PROJECT_SOURCE_DIR}/util/include)
endif()
gtest_discover_tests(tsugumi_rpc_frame_tests TEST_PREFIX "core_rpc_frame."
  DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 30)
if(TSUGUMI_INSTALLED_CORE)
  install(TARGETS tsugumi_rpc_trace_tests tsugumi_rpc_frame_tests RUNTIME DESTINATION libexec/tsugumi-tests)
endif()
