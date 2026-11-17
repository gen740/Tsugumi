include(${CMAKE_CURRENT_LIST_DIR}/remote-result-daemon-sources.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/remote-result-worker-sources.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/remote-result-versions-sources.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/remote-result-runtime-support-sources.cmake)

add_executable(tsugumi_remote_result_tests
  ${_tsugumi_remote_result_daemon_sources}
  ${_tsugumi_remote_result_worker_sources}
  ${_tsugumi_remote_result_versions_sources}
  ${_tsugumi_remote_result_runtime_support_sources}
)
target_include_directories(tsugumi_remote_result_tests PRIVATE ${_core_headers} ${_workerd_headers})
target_link_libraries(tsugumi_remote_result_tests PRIVATE tsugumi_core GTest::gtest_main)
target_compile_options(tsugumi_remote_result_tests PRIVATE -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough)
gtest_discover_tests(tsugumi_remote_result_tests TEST_PREFIX "core_remote." DISCOVERY_MODE PRE_TEST
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../.."
  PROPERTIES TIMEOUT 30)

if(TSUGUMI_INSTALLED_CORE)
  install(TARGETS tsugumi-environment-daemon-double tsugumid-mock tsugumi_agent_rpc_tests tsugumi_rpc_cli_tests tsugumi_remote_result_tests
    RUNTIME DESTINATION libexec/tsugumi-tests)
endif()
