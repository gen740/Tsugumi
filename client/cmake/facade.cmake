# This target is independent of the daemon, Nix and project storage.
add_library(tsugumi_client STATIC src/client.cc src/client_c.cc src/host_views.cc)
target_include_directories(tsugumi_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tsugumi_client PUBLIC tsugumi_protocol)
# Encoding and verification are header-only. In a Wasm build the host flatc
# installation supplies headers, never a native archive to the Wasm linker.
get_target_property(_flatbuffers_headers flatbuffers::flatbuffers INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(tsugumi_client SYSTEM PUBLIC ${_flatbuffers_headers})
target_compile_features(tsugumi_client PUBLIC cxx_std_20)
target_compile_options(tsugumi_client PRIVATE -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough)
