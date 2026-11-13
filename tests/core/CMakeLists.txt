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
  set(_test_agent_schema ${TSUGUMI_GEN_DIR}/bfbs/agent.bfbs)
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

# The thought scheduler's timing can also be checked without any sockets.
add_executable(tsugumi_thought_schedule_tests agent/thought_schedule_test.cc)
target_include_directories(tsugumi_thought_schedule_tests PRIVATE ${_core_headers})
target_compile_options(tsugumi_thought_schedule_tests PRIVATE -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough)
add_test(NAME core_thought_schedule COMMAND tsugumi_thought_schedule_tests)

# Product client -> real Agent/core sockets -> controlled external model.
# These scenarios neither include nor link private core implementation APIs.
find_package(GTest CONFIG REQUIRED)
add_executable(tsugumi_agent_rpc_tests agent/rpc_test.cc agent/project_rpc_test.cc agent_fixture_server.cc)
target_link_libraries(tsugumi_agent_rpc_tests PRIVATE tsugumi_client GTest::gtest_main hosq Threads::Threads)
target_compile_options(tsugumi_agent_rpc_tests PRIVATE -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough)
target_compile_definitions(tsugumi_agent_rpc_tests PRIVATE
  TSUGUMI_AGENT_BINARY="$<TARGET_FILE:tsugumi-agentd>"
  TSUGUMI_PROJECT_BINARY="${_test_mock}"
  TSUGUMI_WORKER_BINARY="$<TARGET_FILE:tsugumi-worker>"
  TSUGUMI_TEST_SDK="${_test_sdk}")
add_dependencies(tsugumi_agent_rpc_tests tsugumi-agentd tsugumid-mock)
include(GoogleTest)
gtest_discover_tests(tsugumi_agent_rpc_tests TEST_PREFIX "core_agent." DISCOVERY_MODE PRE_TEST
  PROPERTIES TIMEOUT 30)

add_executable(tsugumi_rpc_cli_tests rpc_cli_test.cc agent_fixture_server.cc)
target_link_libraries(tsugumi_rpc_cli_tests PRIVATE GTest::gtest_main hosq Threads::Threads)
target_compile_options(tsugumi_rpc_cli_tests PRIVATE -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough)
target_compile_definitions(tsugumi_rpc_cli_tests PRIVATE
  TSUGUMI_RPC_BINARY="$<TARGET_FILE:tsugumi-rpc>"
  TSUGUMI_AGENT_BINARY="$<TARGET_FILE:tsugumi-agentd>"
  TSUGUMI_PROJECT_BINARY="${_test_mock}"
  TSUGUMI_WORKER_BINARY="$<TARGET_FILE:tsugumi-worker>"
  TSUGUMI_AGENT_SCHEMA="${_test_agent_schema}"
  TSUGUMI_PROJECT_SCHEMA="${_test_project_schema}"
  TSUGUMI_TEST_SDK="${_test_sdk}")
add_dependencies(tsugumi_rpc_cli_tests tsugumi-rpc tsugumi-agentd tsugumid-mock)
gtest_discover_tests(tsugumi_rpc_cli_tests TEST_PREFIX "core_rpc_cli." DISCOVERY_MODE PRE_TEST
  PROPERTIES TIMEOUT 30)

add_executable(tsugumi_remote_result_tests remote_result_test.cc collect_value_test.cc display_module_test.cc layout_test.cc)
target_include_directories(tsugumi_remote_result_tests PRIVATE ${_core_headers})
target_link_libraries(tsugumi_remote_result_tests PRIVATE tsugumi_core GTest::gtest_main)
target_compile_options(tsugumi_remote_result_tests PRIVATE -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough)
target_compile_definitions(tsugumi_remote_result_tests PRIVATE
  TSUGUMI_LAYOUT_FIXTURES="tests/fixtures")
gtest_discover_tests(tsugumi_remote_result_tests TEST_PREFIX "core_remote." DISCOVERY_MODE PRE_TEST
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../.."
  PROPERTIES TIMEOUT 30)

if(TSUGUMI_INSTALLED_CORE)
  install(TARGETS tsugumid-mock tsugumi_agent_rpc_tests tsugumi_rpc_cli_tests tsugumi_remote_result_tests tsugumi_thought_schedule_tests
    RUNTIME DESTINATION libexec/tsugumi-tests)
endif()

add_executable(tsugumi_workerd_tests workerd_test.cc)
target_include_directories(tsugumi_workerd_tests PRIVATE ${_core_headers} ${_workerd_headers})
target_link_libraries(tsugumi_workerd_tests PRIVATE tsugumi_workerd_support)
target_compile_options(tsugumi_workerd_tests PRIVATE -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough)
add_test(NAME workerd_protocol_journal COMMAND tsugumi_workerd_tests)
if(TSUGUMI_INSTALLED_CORE)
  install(TARGETS tsugumi_workerd_tests RUNTIME DESTINATION libexec/tsugumi-tests)
endif()
