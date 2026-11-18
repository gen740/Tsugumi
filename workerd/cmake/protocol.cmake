# The native binary deliberately has no dependency on Nix or remote reception.
add_library(tsugumi_worker_protocol STATIC src/protocol.cc)
target_include_directories(tsugumi_worker_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tsugumi_worker_protocol PUBLIC tsugumi_protocol flatbuffers::flatbuffers)
