target_link_libraries(tsugumi_client PUBLIC tsugumi_client_transport)

add_executable(tsugumi-rpc src/main.cc)
target_link_libraries(tsugumi-rpc PRIVATE tsugumi_client flatbuffers::flatbuffers)
target_compile_options(tsugumi-rpc PRIVATE -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough)
install(TARGETS tsugumi_client tsugumi-rpc ARCHIVE DESTINATION lib RUNTIME DESTINATION bin)
install(DIRECTORY include/ DESTINATION include)
