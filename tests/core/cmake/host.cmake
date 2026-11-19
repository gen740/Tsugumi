include(${CMAKE_CURRENT_LIST_DIR}/host-policy-sources.cmake)
if(NOT TSUGUMI_NATIVE_DARWIN_TESTS)
  include(${CMAKE_CURRENT_LIST_DIR}/host-versions-sources.cmake)
endif()
include(${CMAKE_CURRENT_LIST_DIR}/host-settings-sources.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/host-environment-sources.cmake)

# Storage, Skill reads, environment adapters and thought timing need no sockets.
add_executable(tsugumi_host_settings_tests
  ${_tsugumi_host_policy_sources}
  ${_tsugumi_host_versions_sources}
  ${_tsugumi_host_settings_sources}
  ${_tsugumi_host_environment_sources}
)
target_include_directories(tsugumi_host_settings_tests PRIVATE ${_core_headers})
target_compile_definitions(tsugumi_host_settings_tests PRIVATE
  TSUGUMI_CACHE_PROBE="${_test_guide_root}/hostd/tools/test-binary-cache.py")
if(TSUGUMI_INSTALLED_CORE)
  install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../../hostd/tools/test-binary-cache.py
    DESTINATION share/tsugumi/guide-tests/hostd/tools)
  if(NOT TSUGUMI_NATIVE_DARWIN_TESTS)
    install(FILES
      ${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/layout-invalid.json
      ${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/layout-scenes.json
      ${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/wire-routes.json
      DESTINATION share/tsugumi/guide-tests/tests/fixtures)
  endif()
endif()
target_compile_definitions(tsugumi_host_settings_tests PRIVATE TSUGUMI_LAYOUT_FIXTURES="${_test_guide_root}/tests/fixtures")
target_link_libraries(tsugumi_host_settings_tests PRIVATE tsugumi_host_core tsugumi_core PkgConfig::LIBGIT2 GTest::gtest_main)
target_compile_options(tsugumi_host_settings_tests PRIVATE -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough -Wmissing-designated-field-initializers)
gtest_discover_tests(tsugumi_host_settings_tests TEST_PREFIX "core_host_settings." DISCOVERY_MODE PRE_TEST
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../..")
if(TSUGUMI_INSTALLED_CORE)
  install(TARGETS tsugumi_host_settings_tests RUNTIME DESTINATION libexec/tsugumi-tests)
endif()
