# Typed JSON support shared by the core and the Card worker.
add_library(tsugumi_json STATIC src/schema.cc src/util/text.cc)
target_include_directories(tsugumi_json PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(tsugumi_json PUBLIC hosq)

# The RPC schema is embedded for reflection based copies and request digests.
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/tsugumi_rpc_bfbs.cc
  COMMAND ${CMAKE_COMMAND} -DINPUT=${TSUGUMI_GEN_DIR}/bfbs/tsugumi.bfbs
          -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/tsugumi_rpc_bfbs.cc -DSYMBOL=tsugumi_rpc_bfbs
          -P ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
  DEPENDS ${TSUGUMI_GEN_DIR}/bfbs/tsugumi.bfbs ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
  VERBATIM)

# The Nix headers need C++23; only this file includes them.
add_library(tsugumi_nix_eval STATIC src/nix_eval.cc)
target_compile_features(tsugumi_nix_eval PRIVATE cxx_std_23)
target_link_libraries(tsugumi_nix_eval PRIVATE PkgConfig::NIX Threads::Threads)

add_library(tsugumi_core STATIC
  src/util/crypto.cc
  src/util/fs.cc
  src/util/log.cc
  src/util/process.cc
  src/util/sqlite.cc
  src/types.cc
  src/document.cc
  src/graph.cc
  src/model.cc
  src/wire.cc
  src/store.cc
  src/history.cc
  src/lanes.cc
  src/server.cc
  src/nix.cc
  src/worker_proc.cc
  src/worker_pool.cc
  src/scheduler.cc
  src/service.cc
  src/service_run.cc
  src/daemon.cc
  ${CMAKE_CURRENT_BINARY_DIR}/tsugumi_rpc_bfbs.cc
)
target_include_directories(tsugumi_core
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(tsugumi_core PUBLIC tsugumi_protocol tsugumi_json flatbuffers::flatbuffers
  PkgConfig::SQLITE3 Threads::Threads tsugumi_nix_eval)
if(NOT APPLE)
  target_link_libraries(tsugumi_core PRIVATE PkgConfig::CRYPTO)
endif()

# Values fixed at build time (Nix supplies them for the container image).
set(TSUGUMI_CARD_NIXPKGS_REV "$ENV{TSUGUMI_CARD_NIXPKGS_REV}" CACHE STRING "nixpkgs revision for Cards")
set(TSUGUMI_CARD_NIXPKGS_NARHASH "$ENV{TSUGUMI_CARD_NIXPKGS_NARHASH}" CACHE STRING "nixpkgs NAR hash for Cards")
set(TSUGUMI_BWRAP "" CACHE FILEPATH "bubblewrap used for sandbox: true Cards")
set(TSUGUMI_BASH "/bin/bash" CACHE FILEPATH "bash used for Card shell steps")
set(TSUGUMI_CARD_PATH "/usr/bin:/bin" CACHE STRING "PATH given to Card shell steps")
target_compile_definitions(tsugumi_core PRIVATE
  TSUGUMI_CARD_NIXPKGS_REV="${TSUGUMI_CARD_NIXPKGS_REV}"
  TSUGUMI_CARD_NIXPKGS_NARHASH="${TSUGUMI_CARD_NIXPKGS_NARHASH}"
  TSUGUMI_BWRAP="${TSUGUMI_BWRAP}"
  TSUGUMI_BASH="${TSUGUMI_BASH}"
  TSUGUMI_CARD_PATH="${TSUGUMI_CARD_PATH}")

add_executable(tsugumid src/main.cc)
target_include_directories(tsugumid PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(tsugumid PRIVATE tsugumi_core)
install(TARGETS tsugumid RUNTIME DESTINATION bin)

# Supervisor of the shared VM (Linux guest only): one tsugumid per project.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(tsugumi-vmd src/vmd_main.cc)
  target_link_libraries(tsugumi-vmd PRIVATE Threads::Threads)
  install(TARGETS tsugumi-vmd RUNTIME DESTINATION bin)
endif()
