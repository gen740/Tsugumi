# Card worker host and the SDK files installed for Nix builds of Cards.
add_executable(tsugumi-worker src/worker_main.cpp)
target_include_directories(tsugumi-worker PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tsugumi-worker PRIVATE tsugumi_protocol tsugumi_json flatbuffers::flatbuffers
  Threads::Threads ${CMAKE_DL_LIBS})
install(TARGETS tsugumi-worker RUNTIME DESTINATION bin)
install(DIRECTORY include/ DESTINATION share/tsugumi/sdk/include)
install(DIRECTORY nix/ DESTINATION share/tsugumi/sdk/nix)
install(DIRECTORY python/ DESTINATION share/tsugumi/sdk/python)
install(FILES src/python_main.c DESTINATION share/tsugumi/sdk/src)
