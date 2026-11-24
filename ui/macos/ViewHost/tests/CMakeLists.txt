find_package(GTest CONFIG REQUIRED)

add_executable(tsugumi-view-decode-tests
  decode_test.cc
  "${view_gen_dir}/webgpu_imports.gen.cc"
)
add_dependencies(tsugumi-view-decode-tests tsugumi-webgpu-generated)
target_compile_features(tsugumi-view-decode-tests PRIVATE cxx_std_20)
target_include_directories(tsugumi-view-decode-tests PRIVATE "${view_gen_dir}" "..")
target_include_directories(tsugumi-view-decode-tests SYSTEM PRIVATE "${TSUGUMI_DAWN_DIR}/include")
target_link_libraries(tsugumi-view-decode-tests PRIVATE GTest::gtest_main)
