add_executable(tsugumi_native_contract_tests native_contract_test.cc)
target_include_directories(tsugumi_native_contract_tests PRIVATE ${_core_headers}
  ${_test_sdk}/include ${_test_sdk}/worker/include ${_test_sdk}/worker/src
  ${CMAKE_CURRENT_SOURCE_DIR}/../../workerd/include ${CMAKE_CURRENT_SOURCE_DIR}/../../workerd/src)
target_link_libraries(tsugumi_native_contract_tests PRIVATE tsugumi_worker_protocol tsugumi_json
  flatbuffers::flatbuffers GTest::gtest_main Threads::Threads ${CMAKE_DL_LIBS})
gtest_discover_tests(tsugumi_native_contract_tests TEST_PREFIX "native_contract." DISCOVERY_MODE PRE_TEST)

add_executable(tsugumi_execution_service_tests execution_service_test.cc worker_execution_test.cc local_worker_test.cc ${_local_service_source} doubles/nix_eval.cc)
target_include_directories(tsugumi_execution_service_tests PRIVATE ${_core_headers})
target_compile_options(tsugumi_execution_service_tests PRIVATE -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough -Wmissing-designated-field-initializers)
target_link_libraries(tsugumi_execution_service_tests PRIVATE tsugumi_core GTest::gtest_main)
target_compile_definitions(tsugumi_execution_service_tests PRIVATE
  TSUGUMI_WORKER_BINARY="$<TARGET_FILE:tsugumi-worker>"
  TSUGUMI_TEST_SDK="${_test_sdk}")
add_dependencies(tsugumi_execution_service_tests tsugumi-worker tsugumi-workerd)
gtest_discover_tests(tsugumi_execution_service_tests TEST_PREFIX "native_execution." DISCOVERY_MODE PRE_TEST)

if(TSUGUMI_INSTALLED_CORE)
  install(TARGETS tsugumi_native_contract_tests tsugumi_execution_service_tests RUNTIME DESTINATION libexec/tsugumi-tests)
endif()
