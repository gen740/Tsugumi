# Card worker host and the SDK files installed for Nix builds of Cards.
add_executable(tsugumi-worker src/worker_main.cc)
target_include_directories(tsugumi-worker PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tsugumi-worker PRIVATE tsugumi_protocol tsugumi_json flatbuffers::flatbuffers
  Threads::Threads ${CMAKE_DL_LIBS})
install(TARGETS tsugumi-worker RUNTIME DESTINATION bin)
install(DIRECTORY include/ DESTINATION share/tsugumi/sdk/include)
install(DIRECTORY nix/ DESTINATION share/tsugumi/sdk/nix)
install(DIRECTORY python/ DESTINATION share/tsugumi/sdk/python)
install(DIRECTORY r/ DESTINATION share/tsugumi/sdk/r)
install(DIRECTORY julia/ DESTINATION share/tsugumi/sdk/julia)
install(DIRECTORY go/ DESTINATION share/tsugumi/sdk/go)
install(FILES src/python_main.c src/go_host.c src/card_runtime.cc src/worker_main.cc DESTINATION share/tsugumi/sdk/src)
# What else the worker is built from, for a Card of another system: its Nix
# build (sdk/nix/default.nix, `worker`) compiles the worker from these with
# the Card's own nixpkgs, so the worker and the Card's main.so share one libc.
install(FILES ${PROJECT_SOURCE_DIR}/core/src/schema.hh ${PROJECT_SOURCE_DIR}/core/src/schema.cc
  ${PROJECT_SOURCE_DIR}/core/src/message_json.hh DESTINATION share/tsugumi/sdk/worker/core)
install(FILES ${PROJECT_SOURCE_DIR}/core/src/util/text.hh ${PROJECT_SOURCE_DIR}/core/src/util/text.cc
  DESTINATION share/tsugumi/sdk/worker/core/util)
install(FILES ${TSUGUMI_PROTOCOL_DIR}/worker.fbs ${TSUGUMI_PROTOCOL_DIR}/data.fbs
  ${TSUGUMI_PROTOCOL_DIR}/tsugumi_enums.fbs DESTINATION share/tsugumi/sdk/worker/protocol)
install(DIRECTORY ${HOSQ_INCLUDE_DIR}/hosq DESTINATION share/tsugumi/sdk/worker/include)
install(FILES ${HOSQ_INCLUDE_DIR}/../LICENSE DESTINATION share/tsugumi/sdk/worker/include/hosq RENAME LICENSE OPTIONAL)
install(DIRECTORY codegen/ DESTINATION share/tsugumi/sdk/codegen)
