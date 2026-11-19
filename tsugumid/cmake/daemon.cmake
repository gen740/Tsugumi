include(${CMAKE_CURRENT_LIST_DIR}/util-sources.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/versions-sources.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/worker-sources.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/transport-sources.cmake)

add_library(tsugumi_core STATIC
  ${_tsugumi_core_util_sources}
  ${PROJECT_SOURCE_DIR}/tsugumid/runtime/src/permissions.cc
  ${_tsugumi_core_versions_sources}
  ${_tsugumi_core_worker_sources}
  ${PROJECT_SOURCE_DIR}/tsugumid/runtime/src/store.cc
  ${PROJECT_SOURCE_DIR}/tsugumid/runtime/src/history.cc
  ${_tsugumi_core_transport_sources}
  ${PROJECT_SOURCE_DIR}/tsugumid/runtime/src/nix.cc
  runtime/src/run_scratch.cc
  ${PROJECT_SOURCE_DIR}/tsugumid/runtime/src/collect_value.cc
  ${PROJECT_SOURCE_DIR}/tsugumid/runtime/src/execution_service.cc
  ${PROJECT_SOURCE_DIR}/tsugumid/runtime/src/remote_job.cc
  ${PROJECT_SOURCE_DIR}/tsugumid/runtime/src/remote_invocation.cc
  ${PROJECT_SOURCE_DIR}/tsugumid/runtime/src/remote_execution_service.cc
  ${PROJECT_SOURCE_DIR}/tsugumid/runtime/src/scheduler.cc
  ${PROJECT_SOURCE_DIR}/tsugumid/runtime/src/service.cc
  ${PROJECT_SOURCE_DIR}/tsugumid/runtime/src/service_run.cc
  ${PROJECT_SOURCE_DIR}/tsugumid/runtime/src/service_shell.cc
  ${PROJECT_SOURCE_DIR}/tsugumid/runtime/src/service_prepare.cc
  ${PROJECT_SOURCE_DIR}/tsugumid/runtime/src/daemon.cc
)
target_include_directories(tsugumi_core
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/runtime/src)
target_link_libraries(tsugumi_core PUBLIC tsugumi_trace_codec tsugumi_rpc_schema tsugumi_transport tsugumi_document tsugumi_protocol tsugumi_json flatbuffers::flatbuffers
  PkgConfig::SQLITE3 PkgConfig::LIBGIT2 Threads::Threads tsugumi_workerd_support tsugumi_worker_values tsugumi_runtime_support tsugumi_launch_paths tsugumi_nix_store)
# Embed the shared worker engine once in the installed core archive. It uses
# neutral value/wire helpers supplied by its caller, never scheduler/service APIs.
target_link_libraries(tsugumi_core PRIVATE tsugumi_worker_runtime)
if(NOT APPLE)
  target_link_libraries(tsugumi_core PRIVATE PkgConfig::CRYPTO)
endif()

# Not a Card's shell: that one, and its PATH, come from the Card's own manifest.
set(TSUGUMI_BASH "/bin/bash" CACHE FILEPATH "bash of the project shell commands (the Shell RPC)")
target_compile_definitions(tsugumi_core PRIVATE
  TSUGUMI_CARD_NIXPKGS_REV="${TSUGUMI_CARD_NIXPKGS_REV}"
  TSUGUMI_CARD_NIXPKGS_NARHASH="${TSUGUMI_CARD_NIXPKGS_NARHASH}"
  TSUGUMI_BWRAP="${TSUGUMI_BWRAP}"
  TSUGUMI_BASH="${TSUGUMI_BASH}")

add_executable(tsugumid ${PROJECT_SOURCE_DIR}/tsugumid/runtime/src/main.cc)
add_dependencies(tsugumid tsugumi-workerd)
target_include_directories(tsugumid PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/runtime/src)
# Executables choose the evaluator implementation. Tests link their double
# instead, without relying on a linker's archive extraction or symbol precedence.
target_link_libraries(tsugumid PRIVATE tsugumi_core tsugumi_nix_eval)
install(TARGETS tsugumid RUNTIME DESTINATION bin)
