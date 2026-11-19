# Embedded evaluator checks build tiny text derivations, never start a worker.
add_executable(tsugumi_definition_evaluation_tests definition_evaluation_test.cc)
target_include_directories(tsugumi_definition_evaluation_tests PRIVATE ${_core_headers})
target_link_libraries(tsugumi_definition_evaluation_tests PRIVATE tsugumi_core tsugumi_nix_eval GTest::gtest)
target_compile_options(tsugumi_definition_evaluation_tests PRIVATE -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough -Wmissing-designated-field-initializers)
gtest_discover_tests(tsugumi_definition_evaluation_tests TEST_PREFIX "definition_evaluation." DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120)
if(TSUGUMI_INSTALLED_CORE)
  install(TARGETS tsugumi_definition_evaluation_tests RUNTIME DESTINATION libexec/tsugumi-tests)
endif()
