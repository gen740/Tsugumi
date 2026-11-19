# The worker condition seam is independent of services and scheduling.
add_executable(tsugumi_worker_condition_tests worker_condition_test.cc)
target_include_directories(tsugumi_worker_condition_tests PRIVATE ${_core_headers})
target_link_libraries(tsugumi_worker_condition_tests PRIVATE tsugumi_core GTest::gtest_main)
target_compile_options(tsugumi_worker_condition_tests PRIVATE
  -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough -Wmissing-designated-field-initializers)
gtest_discover_tests(tsugumi_worker_condition_tests TEST_PREFIX "worker_condition."
  DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 30)
if(TSUGUMI_INSTALLED_CORE)
  install(TARGETS tsugumi_worker_condition_tests RUNTIME DESTINATION libexec/tsugumi-tests)
endif()
