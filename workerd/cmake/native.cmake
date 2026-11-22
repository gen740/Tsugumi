add_executable(tsugumi-worker src/native_main.cc)
target_include_directories(tsugumi-worker PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tsugumi-worker PRIVATE tsugumi_protocol tsugumi_frame flatbuffers::flatbuffers
  Threads::Threads ${CMAKE_DL_LIBS})
install(TARGETS tsugumi-worker RUNTIME DESTINATION bin)
