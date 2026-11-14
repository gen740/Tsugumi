if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.28)
  if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "26.0" CACHE STRING "")
  endif()
  project(TsugumiCoreTests LANGUAGES CXX)
  include(cmake/installed-core.cmake)
else()
  set(_workerd_headers ${PROJECT_SOURCE_DIR}/workerd/src)
  set(_core_main ${PROJECT_SOURCE_DIR}/core/src/main.cc)
  set(_core_headers ${PROJECT_SOURCE_DIR}/core/src)
  set(_test_mock "$<TARGET_FILE:tsugumid-mock>")
  set(_test_sdk ${PROJECT_SOURCE_DIR}/sdk)
  set(_test_project_schema ${TSUGUMI_GEN_DIR}/bfbs/tsugumi.bfbs)
endif()

# The real core serves its public RPC. Only the external Nix evaluator is
# replaced: defining its symbols in the executable keeps the real evaluator's
# object out of the static library link. This is installed only by ci-core.
add_executable(tsugumid-mock
  ${_core_main}
  doubles/nix_eval.cc)
target_include_directories(tsugumid-mock PRIVATE ${_core_headers})
target_link_libraries(tsugumid-mock PRIVATE tsugumi_core)
add_dependencies(tsugumid-mock tsugumid tsugumi-worker)

if(NOT TSUGUMI_INSTALLED_CORE)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME core_rpc
    COMMAND ${Python3_EXECUTABLE} -m pytest -q --core-report -m "not environment" ${CMAKE_CURRENT_SOURCE_DIR})
  set_tests_properties(core_rpc PROPERTIES
    TIMEOUT 300
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    ENVIRONMENT "TSUGUMI_BUILD_DIR=${PROJECT_BINARY_DIR};TSUGUMI_PY_BINDINGS=${TSUGUMI_GEN_DIR}/python;TSUGUMI_MOCK_DAEMON=$<TARGET_FILE:tsugumid-mock>;TSUGUMID=$<TARGET_FILE:tsugumid>;TSUGUMI_WORKER=$<TARGET_FILE:tsugumi-worker>")
endif()

# Product client -> real Agent/core sockets -> controlled external model.
# These scenarios neither include nor link private core implementation APIs.
find_package(GTest CONFIG REQUIRED)
if(TSUGUMI_INSTALLED_CORE)
  set(_test_environment_daemon ${CMAKE_INSTALL_PREFIX}/libexec/tsugumi-tests/tsugumi-environment-daemon-double)
  # Nix build sources are temporary; installed tests own their guide fixtures.
  set(_test_guide_root ${CMAKE_INSTALL_PREFIX}/share/tsugumi/guide-tests)
  install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../agent/Shared/guide-script.json
    ${CMAKE_CURRENT_SOURCE_DIR}/../../agent/Shared/guide-script.en.json
    DESTINATION share/tsugumi/guide-tests/agent/Shared
    COMPONENT guide-test-resources)
  install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../../demos
    DESTINATION share/tsugumi/guide-tests
    COMPONENT guide-test-resources)
else()
  set(_test_environment_daemon "$<TARGET_FILE:tsugumi-environment-daemon-double>")
  set(_test_guide_root ${PROJECT_SOURCE_DIR})
endif()
add_executable(tsugumi_agent_rpc_tests agent/rpc_test.cc agent/host_rpc_test.cc agent/environment_rpc_test.cc agent/skills_rpc_test.cc agent/models_rpc_test.cc agent/settings_rpc_test.cc agent/guide_rpc_test.cc agent/execution_broker_test.cc agent/project_rpc_test.cc agent/version_note_rpc_test.cc agent_fixture_server.cc)
target_link_libraries(tsugumi_agent_rpc_tests PRIVATE tsugumi_client tsugumi_worker_protocol tsugumi_peer_tls GTest::gtest_main hosq Threads::Threads)
target_compile_options(tsugumi_agent_rpc_tests PRIVATE -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough)
target_compile_definitions(tsugumi_agent_rpc_tests PRIVATE
  TSUGUMI_GUIDE_TEST_ROOT="${_test_guide_root}"
  TSUGUMI_ENVIRONMENT_DAEMON="${_test_environment_daemon}"
  TSUGUMI_AGENT_BINARY="$<TARGET_FILE:tsugumi-agentd>"
  TSUGUMI_PROJECT_BINARY="${_test_mock}"
  TSUGUMI_WORKER_BINARY="$<TARGET_FILE:tsugumi-worker>"
  TSUGUMI_TEST_SDK="${_test_sdk}")
add_executable(tsugumi-environment-daemon-double doubles/environment_daemon.cc)
target_link_libraries(tsugumi-environment-daemon-double PRIVATE tsugumi_agent_core)
target_compile_options(tsugumi-environment-daemon-double PRIVATE -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough)
add_dependencies(tsugumi_agent_rpc_tests tsugumi-agentd tsugumid-mock tsugumi-environment-daemon-double)
include(GoogleTest)
gtest_discover_tests(tsugumi_agent_rpc_tests TEST_PREFIX "core_agent." DISCOVERY_MODE PRE_TEST
  PROPERTIES TIMEOUT 30)

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

add_executable(tsugumi_remote_result_tests remote_result_test.cc collect_value_test.cc display_module_test.cc version_note_test.cc cache_usage_test.cc history_copy_test.cc module_source_test.cc layout_test.cc card_identity_test.cc)
target_include_directories(tsugumi_remote_result_tests PRIVATE ${_core_headers})
target_link_libraries(tsugumi_remote_result_tests PRIVATE tsugumi_core GTest::gtest_main)
target_compile_options(tsugumi_remote_result_tests PRIVATE -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough)
target_compile_definitions(tsugumi_remote_result_tests PRIVATE
  TSUGUMI_LAYOUT_FIXTURES="tests/fixtures")
gtest_discover_tests(tsugumi_remote_result_tests TEST_PREFIX "core_remote." DISCOVERY_MODE PRE_TEST
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../.."
  PROPERTIES TIMEOUT 30)

if(TSUGUMI_INSTALLED_CORE)
  install(TARGETS tsugumi-environment-daemon-double tsugumid-mock tsugumi_agent_rpc_tests tsugumi_rpc_cli_tests tsugumi_remote_result_tests
    RUNTIME DESTINATION libexec/tsugumi-tests)
endif()

add_executable(tsugumi_workerd_tests workerd_test.cc)
target_include_directories(tsugumi_workerd_tests PRIVATE ${_core_headers} ${_workerd_headers})
target_link_libraries(tsugumi_workerd_tests PRIVATE tsugumi_workerd_support)
target_compile_options(tsugumi_workerd_tests PRIVATE -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough)
add_test(NAME workerd_protocol_journal COMMAND tsugumi_workerd_tests)

add_executable(tsugumi_worker_staging_tests worker_staging_test.cc)
target_include_directories(tsugumi_worker_staging_tests PRIVATE ${_core_headers})
target_link_libraries(tsugumi_worker_staging_tests PRIVATE tsugumi_workerd_support Threads::Threads)
target_compile_options(tsugumi_worker_staging_tests PRIVATE -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough)
add_test(NAME worker_staging COMMAND tsugumi_worker_staging_tests)
set_tests_properties(worker_staging PROPERTIES TIMEOUT 15)
if(TSUGUMI_INSTALLED_CORE)
  install(TARGETS tsugumi_workerd_tests tsugumi_worker_staging_tests RUNTIME DESTINATION libexec/tsugumi-tests)
endif()

add_executable(tsugumi_peer_tls_tests peer_tls_test.cc agent_fixture_server.cc)
target_link_libraries(tsugumi_peer_tls_tests PRIVATE tsugumi_peer_tls GTest::gtest_main hosq Threads::Threads)
target_compile_options(tsugumi_peer_tls_tests PRIVATE -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough)
gtest_discover_tests(tsugumi_peer_tls_tests TEST_PREFIX "peer_tls." DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 15)
if(TSUGUMI_INSTALLED_CORE)
  install(TARGETS tsugumi_peer_tls_tests RUNTIME DESTINATION libexec/tsugumi-tests)
endif()

# Storage, Skill reads, environment adapters and thought timing need no sockets.
add_executable(tsugumi_host_settings_tests agent/catalog_test.cc agent/helper_scope_test.cc agent/settings_store_test.cc agent/skills_read_test.cc agent/environment_adapter_test.cc agent/thought_schedule_test.cc agent_fixture_server.cc)
target_include_directories(tsugumi_host_settings_tests PRIVATE ${_core_headers})
target_link_libraries(tsugumi_host_settings_tests PRIVATE tsugumi_agent_core GTest::gtest_main)
target_compile_options(tsugumi_host_settings_tests PRIVATE -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough)
gtest_discover_tests(tsugumi_host_settings_tests TEST_PREFIX "core_host_settings." DISCOVERY_MODE PRE_TEST)
if(TSUGUMI_INSTALLED_CORE)
  install(TARGETS tsugumi_host_settings_tests RUNTIME DESTINATION libexec/tsugumi-tests)
endif()
