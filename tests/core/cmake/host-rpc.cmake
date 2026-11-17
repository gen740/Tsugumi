include(${CMAKE_CURRENT_LIST_DIR}/host-rpc-host-sources.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/host-rpc-environment-sources.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/host-rpc-settings-sources.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/host-rpc-versions-sources.cmake)

# Product client -> real Agent/core sockets -> controlled external model.
# These scenarios neither include nor link private core implementation APIs.
if(TSUGUMI_INSTALLED_CORE)
  set(_test_environment_daemon ${CMAKE_INSTALL_PREFIX}/libexec/tsugumi-tests/tsugumi-environment-daemon-double)
  # Nix build sources are temporary; installed tests own their guide fixtures.
  set(_test_guide_root ${CMAKE_INSTALL_PREFIX}/share/tsugumi/guide-tests)
  install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../agent/Shared/guide-script.json
    ${CMAKE_CURRENT_SOURCE_DIR}/../../agent/Shared/guide-script.en.json
    DESTINATION share/tsugumi/guide-tests/agent/Shared
    COMPONENT guide-test-resources)
  install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/service-contract.json
    DESTINATION share/tsugumi/guide-tests/tests/fixtures
    COMPONENT guide-test-resources)
  install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../../demos
    DESTINATION share/tsugumi/guide-tests
    COMPONENT guide-test-resources)
else()
  set(_test_environment_daemon "$<TARGET_FILE:tsugumi-environment-daemon-double>")
  set(_test_guide_root ${PROJECT_SOURCE_DIR})
endif()
add_executable(tsugumi_agent_rpc_tests
  ${_tsugumi_host_rpc_host_sources}
  ${_tsugumi_host_rpc_environment_sources}
  ${_tsugumi_host_rpc_settings_sources}
  ${_tsugumi_host_rpc_versions_sources}
)
target_link_libraries(tsugumi_agent_rpc_tests PRIVATE PkgConfig::SQLITE3 tsugumi_client tsugumi_worker_protocol tsugumi_peer_tls tsugumi_worker_authorization GTest::gtest_main hosq Threads::Threads)
target_compile_options(tsugumi_agent_rpc_tests PRIVATE -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough -Wmissing-designated-field-initializers)
target_compile_definitions(tsugumi_agent_rpc_tests PRIVATE
  TSUGUMI_GUIDE_TEST_ROOT="${_test_guide_root}"
  TSUGUMI_ENVIRONMENT_DAEMON="${_test_environment_daemon}"
  TSUGUMI_HOST_BINARY="$<TARGET_FILE:tsugumi-hostd>"
  TSUGUMI_PROJECT_BINARY="${_test_mock}"
  TSUGUMI_WORKER_BINARY="$<TARGET_FILE:tsugumi-worker>"
  TSUGUMI_TEST_SDK="${_test_sdk}")
add_executable(tsugumi-environment-daemon-double doubles/environment_daemon.cc ${_project_endpoint_source})
target_link_libraries(tsugumi-environment-daemon-double PRIVATE tsugumi_host_core tsugumi_worker_values)
target_compile_options(tsugumi-environment-daemon-double PRIVATE -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough)
add_dependencies(tsugumi_agent_rpc_tests tsugumi-hostd tsugumid-mock tsugumi-environment-daemon-double)
gtest_discover_tests(tsugumi_agent_rpc_tests TEST_PREFIX "core_agent." DISCOVERY_MODE PRE_TEST
  TEST_FILTER "-RealEnvironmentFixture.AuthenticatesTheRealProjectServiceBeforeReady"
  PROPERTIES TIMEOUT 30)
# This scenario follows the real environment's startup and cleanup budget.
gtest_discover_tests(tsugumi_agent_rpc_tests TEST_PREFIX "core_agent." DISCOVERY_MODE PRE_TEST
  TEST_FILTER "RealEnvironmentFixture.AuthenticatesTheRealProjectServiceBeforeReady"
  TEST_LIST real_environment_tests
  PROPERTIES TIMEOUT 120)
