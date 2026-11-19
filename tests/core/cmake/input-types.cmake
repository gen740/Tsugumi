# Input descriptions share the core resolver and Host presentation without sockets.
add_executable(tsugumi_input_types_tests input_types_test.cc doubles/nix_eval.cc)
target_include_directories(tsugumi_input_types_tests PRIVATE ${_core_headers})
target_link_libraries(tsugumi_input_types_tests PRIVATE tsugumi_host_core tsugumi_core GTest::gtest)
gtest_discover_tests(tsugumi_input_types_tests TEST_PREFIX "input_types." DISCOVERY_MODE PRE_TEST)
if(TSUGUMI_INSTALLED_CORE)
  install(TARGETS tsugumi_input_types_tests RUNTIME DESTINATION libexec/tsugumi-tests)
endif()
