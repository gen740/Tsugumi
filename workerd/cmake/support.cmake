add_library(tsugumi_workerd_support STATIC src/journal.cc src/paths.cc
  ${PROJECT_SOURCE_DIR}/core/src/util/crypto.cc ${PROJECT_SOURCE_DIR}/core/src/util/fs.cc
  src/staging.cc)
target_include_directories(tsugumi_workerd_support PRIVATE ${PROJECT_SOURCE_DIR}/core/src
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tsugumi_workerd_support PUBLIC tsugumi_worker_protocol tsugumi_json)
if(NOT APPLE)
  target_link_libraries(tsugumi_workerd_support PRIVATE PkgConfig::CRYPTO)
endif()
