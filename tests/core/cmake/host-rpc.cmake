include(${CMAKE_CURRENT_LIST_DIR}/host-rpc-host-sources.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/host-rpc-environment-sources.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/host-rpc-settings-sources.cmake)
if(NOT TSUGUMI_NATIVE_DARWIN_TESTS)
  include(${CMAKE_CURRENT_LIST_DIR}/host-rpc-versions-sources.cmake)
endif()

# Product client -> real Agent/core sockets -> controlled external model.
# Only the production result-budget constants need owned Host headers; these
# scenarios do not link the Host implementation.
if(TSUGUMI_INSTALLED_CORE)
  set(_test_environment_daemon ${CMAKE_INSTALL_PREFIX}/libexec/tsugumi-tests/tsugumi-environment-daemon-double)
  # Nix build sources are temporary; installed tests own their guide fixtures.
  set(_test_guide_root ${CMAKE_INSTALL_PREFIX}/share/tsugumi/guide-tests)
  if(NOT TSUGUMI_NATIVE_DARWIN_TESTS AND NOT TSUGUMI_TEST_TARGET STREQUAL "tsugumi_arm_shell_tests")
    install(FILES
      ${CMAKE_CURRENT_SOURCE_DIR}/../../hostd/agent/Shared/guide-script.json
      ${CMAKE_CURRENT_SOURCE_DIR}/../../hostd/agent/Shared/guide-script.en.json
      DESTINATION share/tsugumi/guide-tests/hostd/agent/Shared
      COMPONENT guide-test-resources)
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../../demos
      DESTINATION share/tsugumi/guide-tests
      COMPONENT guide-test-resources)
  endif()
else()
  set(_test_environment_daemon "$<TARGET_FILE:tsugumi-environment-daemon-double>")
  set(_test_guide_root ${PROJECT_SOURCE_DIR})
endif()
set(_agent_rpc_target tsugumi_agent_rpc_tests)
if(TSUGUMI_TEST_TARGET STREQUAL "tsugumi_arm_shell_tests")
  set(_agent_rpc_target tsugumi_arm_shell_tests)
  add_executable(${_agent_rpc_target} agent/project_rpc_test.cc agent_fixture_server.cc)
else()
  add_executable(${_agent_rpc_target}
    ${_tsugumi_host_rpc_host_sources}
    ${_tsugumi_host_rpc_environment_sources}
    ${_tsugumi_host_rpc_settings_sources}
    ${_tsugumi_host_rpc_versions_sources}
  )
endif()
target_include_directories(${_agent_rpc_target} PRIVATE $<TARGET_PROPERTY:tsugumi_host_core,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(${_agent_rpc_target} PRIVATE tsugumi_document tsugumi_json PkgConfig::SQLITE3 tsugumi_client tsugumi_worker_protocol tsugumi_peer_tls GTest::gtest_main hosq Threads::Threads)
target_compile_options(${_agent_rpc_target} PRIVATE -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough -Wmissing-designated-field-initializers)
target_compile_definitions(${_agent_rpc_target} PRIVATE
  TSUGUMI_GUIDE_TEST_ROOT="${_test_guide_root}"
  TSUGUMI_ENVIRONMENT_DAEMON="${_test_environment_daemon}"
  TSUGUMI_HOST_BINARY="$<TARGET_FILE:tsugumi-hostd>"
  TSUGUMI_PROJECT_BINARY="${_test_mock}"
  TSUGUMI_WORKER_BINARY="$<TARGET_FILE:tsugumi-worker>"
  TSUGUMI_TEST_NIX="${_test_nix}")
if(TSUGUMI_INSTALLED_CORE)
  install(TARGETS ${_agent_rpc_target} RUNTIME DESTINATION libexec/tsugumi-tests)
endif()
if(TSUGUMI_TEST_TARGET STREQUAL "tsugumi_arm_shell_tests")
  return()
endif()
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
