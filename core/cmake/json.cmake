# Typed JSON support shared by the core and the Card worker.
add_library(tsugumi_json STATIC src/schema.cc src/util/text.cc)
target_include_directories(tsugumi_json PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(tsugumi_json PUBLIC hosq)
