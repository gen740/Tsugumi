add_library(tsugumi_runtime_support STATIC src/cache_usage.cc)
target_include_directories(tsugumi_runtime_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tsugumi_runtime_support PUBLIC tsugumi_protocol tsugumi_util
  PRIVATE flatbuffers::flatbuffers)
target_include_directories(tsugumi_runtime_support
  PRIVATE "$<TARGET_PROPERTY:tsugumi_nix_store,INTERFACE_INCLUDE_DIRECTORIES>")
