add_library(tsugumi_json STATIC src/util/json.cc src/util/text.cc)
target_include_directories(tsugumi_json PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
tsugumi_owned_headers(tsugumi_json json util/json.hh util/json_object.hh util/text.hh)
target_link_libraries(tsugumi_json PUBLIC hosq)

add_library(tsugumi_util STATIC src/util/crypto.cc src/util/fs.cc src/util/log.cc src/util/process.cc)
target_include_directories(tsugumi_util PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
tsugumi_owned_headers(tsugumi_util util util/crypto.hh util/fs.hh util/log.hh util/process.hh util/sqlite.hh util/sparse.hh)
target_link_libraries(tsugumi_util PUBLIC tsugumi_json Threads::Threads)
if(NOT APPLE)
  target_link_libraries(tsugumi_util PRIVATE PkgConfig::CRYPTO)
endif()

tsugumi_owned_headers(tsugumi_peer_tls tls util/peer_tls.hh)
