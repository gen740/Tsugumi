add_library(tsugumi_json STATIC src/json.cc)
target_include_directories(tsugumi_json PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tsugumi_json PUBLIC hosq tsugumi_util)

add_library(tsugumi_util STATIC src/text.cc src/fs.cc src/log.cc src/process.cc src/sqlite.cc)
target_include_directories(tsugumi_util PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tsugumi_util PUBLIC tsugumi_crypto Threads::Threads PRIVATE native_sqlite)
