add_library(tsugumi_workerd_support STATIC src/journal.cc src/paths.cc
  src/staging.cc)
target_include_directories(tsugumi_workerd_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tsugumi_workerd_support PUBLIC tsugumi_worker_protocol tsugumi_json tsugumi_util)
if(NOT APPLE)
  target_link_libraries(tsugumi_workerd_support PRIVATE PkgConfig::CRYPTO)
endif()
