include(${CMAKE_CURRENT_LIST_DIR}/util-sources.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/versions-sources.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/worker-sources.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/transport-sources.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/runtime-support-sources.cmake)

add_library(tsugumi_core STATIC
  ${_tsugumi_core_util_sources}
  src/permissions.cc
  ${_tsugumi_core_versions_sources}
  ${_tsugumi_core_worker_sources}
  src/store.cc
  src/history.cc
  ${_tsugumi_core_transport_sources}
  src/nix.cc
  ${_tsugumi_core_runtime_support_sources}
  src/collect_value.cc
  src/execution_service.cc
  src/remote_job.cc
  src/remote_invocation.cc
  src/remote_execution_service.cc
  src/scheduler.cc
  src/service.cc
  src/service_run.cc
  src/service_shell.cc
  src/daemon.cc
)
target_include_directories(tsugumi_core
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/sdk/include)
target_link_libraries(tsugumi_core PUBLIC tsugumi_transport tsugumi_document tsugumi_protocol tsugumi_json flatbuffers::flatbuffers
  PkgConfig::SQLITE3 PkgConfig::LIBGIT2 Threads::Threads tsugumi_workerd_support tsugumi_worker_values tsugumi_runtime_support)
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

add_executable(tsugumid src/main.cc)
add_dependencies(tsugumid tsugumi-workerd)
target_include_directories(tsugumid PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
# Executables choose the evaluator implementation. Tests link their double
# instead, without relying on a linker's archive extraction or symbol precedence.
target_link_libraries(tsugumid PRIVATE tsugumi_core tsugumi_nix_eval)
install(TARGETS tsugumid RUNTIME DESTINATION bin)
