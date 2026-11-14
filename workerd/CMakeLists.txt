# The native binary deliberately has no dependency on Nix or remote reception.
add_library(tsugumi_worker_protocol STATIC src/protocol.cc)
target_include_directories(tsugumi_worker_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tsugumi_worker_protocol PUBLIC tsugumi_protocol flatbuffers::flatbuffers)

add_library(tsugumi_workerd_support STATIC src/journal.cc src/paths.cc
  ${PROJECT_SOURCE_DIR}/core/src/util/crypto.cc ${PROJECT_SOURCE_DIR}/core/src/util/fs.cc
  ${PROJECT_SOURCE_DIR}/core/src/util/staging.cc)
target_include_directories(tsugumi_workerd_support PRIVATE ${PROJECT_SOURCE_DIR}/core/src
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tsugumi_workerd_support PUBLIC tsugumi_worker_protocol tsugumi_json)
if(NOT APPLE)
  target_link_libraries(tsugumi_workerd_support PRIVATE PkgConfig::CRYPTO)
endif()

add_executable(tsugumi-worker src/native_main.cc)
target_include_directories(tsugumi-worker PRIVATE ${PROJECT_SOURCE_DIR}/sdk/include ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tsugumi-worker PRIVATE tsugumi_protocol tsugumi_json flatbuffers::flatbuffers
  Threads::Threads ${CMAKE_DL_LIBS})
install(TARGETS tsugumi-worker RUNTIME DESTINATION bin)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_library(tsugumi_remote_store STATIC src/nix_store.cc)
  target_compile_features(tsugumi_remote_store PRIVATE cxx_std_23)
  target_include_directories(tsugumi_remote_store PRIVATE ${PROJECT_SOURCE_DIR}/core/src)
  target_link_libraries(tsugumi_remote_store PRIVATE PkgConfig::NIX)
  add_executable(tsugumi-remote-worker src/remote_main.cc src/invocation.cc)
  target_compile_definitions(tsugumi-remote-worker PRIVATE _GNU_SOURCE)
  target_include_directories(tsugumi-remote-worker PRIVATE ${PROJECT_SOURCE_DIR}/core/src)
  target_link_libraries(tsugumi-remote-worker PRIVATE tsugumi_core tsugumi_workerd_support tsugumi_remote_store)
  install(TARGETS tsugumi-remote-worker RUNTIME DESTINATION bin)
endif()

# Cross-system Cards rebuild this light worker with their own nixpkgs/libc.
install(FILES src/native_main.cc DESTINATION share/tsugumi/sdk/worker/src)
install(FILES ${PROJECT_SOURCE_DIR}/core/src/schema.hh ${PROJECT_SOURCE_DIR}/core/src/schema.cc
  DESTINATION share/tsugumi/sdk/worker/core)
install(FILES ${PROJECT_SOURCE_DIR}/core/src/util/text.hh ${PROJECT_SOURCE_DIR}/core/src/util/text.cc
  DESTINATION share/tsugumi/sdk/worker/core/util)
install(FILES ${TSUGUMI_PROTOCOL_DIR}/worker.fbs ${TSUGUMI_PROTOCOL_DIR}/data.fbs
  ${TSUGUMI_PROTOCOL_DIR}/tsugumi_enums.fbs DESTINATION share/tsugumi/sdk/worker/protocol)
install(DIRECTORY ${HOSQ_INCLUDE_DIR}/hosq DESTINATION share/tsugumi/sdk/worker/include)
install(FILES ${HOSQ_INCLUDE_DIR}/../LICENSE DESTINATION share/tsugumi/sdk/worker/include/hosq RENAME LICENSE OPTIONAL)

install(FILES include/tsugumi/workerd/frame.hh include/tsugumi/workerd/display.hh DESTINATION share/tsugumi/sdk/worker/include/tsugumi/workerd)
