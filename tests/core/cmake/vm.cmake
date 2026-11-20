# Only transport/host lifetime seams; no VM or Nix evaluator is started.
add_executable(tsugumi_vm_lifetime_tests vm_lifetime_test.cc vm_guest_wire_test.cc)
target_include_directories(tsugumi_vm_lifetime_tests PRIVATE ${_core_headers})
target_link_libraries(tsugumi_vm_lifetime_tests PRIVATE tsugumi_host_core tsugumi_client GTest::gtest_main)
gtest_discover_tests(tsugumi_vm_lifetime_tests TEST_PREFIX "vm_lifetime." DISCOVERY_MODE PRE_TEST)
if(TSUGUMI_INSTALLED_CORE)
  install(TARGETS tsugumi_vm_lifetime_tests RUNTIME DESTINATION libexec/tsugumi-tests)
endif()
