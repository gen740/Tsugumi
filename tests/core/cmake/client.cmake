add_executable(tsugumi_rpc_cli_tests rpc_cli_test.cc agent_fixture_server.cc)
target_link_libraries(tsugumi_rpc_cli_tests PRIVATE GTest::gtest_main hosq Threads::Threads)
target_compile_options(tsugumi_rpc_cli_tests PRIVATE -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough)
target_compile_definitions(tsugumi_rpc_cli_tests PRIVATE
  TSUGUMI_RPC_BINARY="$<TARGET_FILE:tsugumi-rpc>"
  TSUGUMI_PROJECT_BINARY="${_test_mock}"
  TSUGUMI_WORKER_BINARY="$<TARGET_FILE:tsugumi-worker>"
  TSUGUMI_PROJECT_SCHEMA="${_test_project_schema}"
  TSUGUMI_TEST_SDK="${_test_sdk}")
add_dependencies(tsugumi_rpc_cli_tests tsugumi-rpc tsugumid-mock)
gtest_discover_tests(tsugumi_rpc_cli_tests TEST_PREFIX "core_rpc_cli." DISCOVERY_MODE PRE_TEST
  PROPERTIES TIMEOUT 30)
