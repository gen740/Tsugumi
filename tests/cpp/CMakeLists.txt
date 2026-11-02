add_executable(tsugumi_core_tests core_tests.cpp)
target_include_directories(tsugumi_core_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/src)
target_link_libraries(tsugumi_core_tests PRIVATE tsugumi_core)
add_test(NAME core_tests COMMAND tsugumi_core_tests)
