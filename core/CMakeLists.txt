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

# Nix integration units use C++23; the rest of the core never includes Nix headers.
add_library(tsugumi_nix_eval STATIC src/nix_eval.cc src/derivation_transport.cc)
target_compile_features(tsugumi_nix_eval PRIVATE cxx_std_23)
target_link_libraries(tsugumi_nix_eval PRIVATE PkgConfig::NIX Threads::Threads)

add_library(tsugumi_core STATIC
  src/util/log.cc
  src/util/process.cc
  src/util/sqlite.cc
  src/types.cc
  src/document.cc
  src/graph.cc
  src/nix_refs.cc
  src/permissions.cc
  src/versions.cc
  src/model.cc
  src/module_source.cc
  src/wire.cc
  src/store.cc
  src/history.cc
  src/lanes.cc
  src/server.cc
  src/nix.cc
  src/run_scratch.cc
  src/cache_usage.cc
  src/worker_proc.cc
  src/worker_pool.cc
  src/collect_value.cc
  src/execution_service.cc
  src/remote_job.cc
  src/remote_invocation.cc
  src/remote_execution_service.cc
  src/scheduler.cc
  src/layout.cc
  src/layout_place.cc
  src/layout_wires.cc
  src/service_layout.cc
  src/service.cc
  src/service_run.cc
  src/service_shell.cc
  src/daemon.cc
  ${CMAKE_CURRENT_BINARY_DIR}/tsugumi_rpc_bfbs.cc
)
set_source_files_properties(src/layout.cc src/layout_place.cc src/layout_wires.cc
  PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")

target_include_directories(tsugumi_core
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/sdk/include)
target_link_libraries(tsugumi_core PUBLIC tsugumi_protocol tsugumi_json flatbuffers::flatbuffers
  PkgConfig::SQLITE3 PkgConfig::LIBGIT2 Threads::Threads tsugumi_nix_eval tsugumi_workerd_support)
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

# Agent state and model execution live in the core, independently of project
# lifetimes. Frontends use the separate agent.fbs and host.fbs contracts.
set(_agent_prompts "")
foreach(prompt system-prompt guide-prompt demo-skill)
  string(REPLACE "-" "_" symbol ${prompt})
  set(output ${CMAKE_CURRENT_BINARY_DIR}/agent_${symbol}.cc)
  add_custom_command(OUTPUT ${output}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${PROJECT_SOURCE_DIR}/agent/Shared/${prompt}.md
      -DOUTPUT=${output} -DSYMBOL=tsugumi_agent_${symbol}
      -P ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS ${PROJECT_SOURCE_DIR}/agent/Shared/${prompt}.md ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
    VERBATIM)
  list(APPEND _agent_prompts ${output})
endforeach()
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/agent_settings.cc
  COMMAND ${CMAKE_COMMAND} -DINPUT=${PROJECT_SOURCE_DIR}/agent/Shared/agent.json
    -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/agent_settings.cc -DSYMBOL=tsugumi_agent_settings
    -P ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
  DEPENDS ${PROJECT_SOURCE_DIR}/agent/Shared/agent.json ${PROJECT_SOURCE_DIR}/cmake/embed.cmake VERBATIM)
foreach(language ja en)
  if(language STREQUAL "en")
    set(script guide-script.en.json)
  else()
    set(script guide-script.json)
  endif()
  set(output ${CMAKE_CURRENT_BINARY_DIR}/guide_script_${language}.cc)
  add_custom_command(OUTPUT ${output}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${PROJECT_SOURCE_DIR}/agent/Shared/${script}
      -DOUTPUT=${output} -DSYMBOL=tsugumi_guide_script_${language}
      -P ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS ${PROJECT_SOURCE_DIR}/agent/Shared/${script} ${PROJECT_SOURCE_DIR}/cmake/embed.cmake VERBATIM)
  list(APPEND _agent_prompts ${output})
endforeach()
add_library(tsugumi_agent_core STATIC
  ${CMAKE_CURRENT_BINARY_DIR}/agent_settings.cc
  ${_agent_prompts}
  src/server.cc
  src/lanes.cc
  src/util/crypto.cc
  src/util/fs.cc
  src/util/log.cc
  src/util/sqlite.cc
  ${CMAKE_CURRENT_BINARY_DIR}/tsugumi_rpc_bfbs.cc
  src/agent/catalog.cc
  src/agent/provider.cc
  src/agent/runtime.cc
  src/agent/tool_images.cc
  src/agent/wire.cc
  src/agent/host.cc
  src/host/comments.cc
  src/host/save.cc
  src/host/guide.cc
  src/host/settings.cc
  src/host/skills.cc
  src/host/projects.cc
  src/host/execution_broker.cc
  src/host/environment.cc
  src/host/environment_native.cc
  src/host/vm_process.cc)
if(APPLE)
  target_link_libraries(tsugumi_agent_core PRIVATE "-framework CoreFoundation")
else()
  find_package(ICU REQUIRED COMPONENTS uc i18n)
  target_link_libraries(tsugumi_agent_core PRIVATE ICU::uc ICU::i18n)
endif()
target_compile_definitions(tsugumi_agent_core PRIVATE
  TSUGUMI_CARD_NIXPKGS_REV="${TSUGUMI_CARD_NIXPKGS_REV}"
  TSUGUMI_CARD_NIXPKGS_NARHASH="${TSUGUMI_CARD_NIXPKGS_NARHASH}")
target_include_directories(tsugumi_agent_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(tsugumi_agent_core PRIVATE -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough)
set(TSUGUMI_AGENT_SQLITE_LIBRARY "" CACHE FILEPATH "SQLite library used by the native Agent service")
target_link_libraries(tsugumi_agent_core PUBLIC tsugumi_protocol tsugumi_json flatbuffers::flatbuffers
  Threads::Threads tsugumi_client PkgConfig::LIBGIT2 CURL::libcurl tsugumi_worker_protocol tsugumi_peer_tls)
if(TSUGUMI_AGENT_SQLITE_LIBRARY)
  target_link_libraries(tsugumi_agent_core PRIVATE ${TSUGUMI_AGENT_SQLITE_LIBRARY})
else()
  target_link_libraries(tsugumi_agent_core PRIVATE PkgConfig::SQLITE3)
endif()
if(NOT APPLE)
  target_link_libraries(tsugumi_agent_core PRIVATE PkgConfig::CRYPTO)
endif()
add_executable(tsugumi-agentd src/agent/main.cc)
target_link_libraries(tsugumi-agentd PRIVATE tsugumi_agent_core)
install(TARGETS tsugumi-agentd RUNTIME DESTINATION bin)
install(FILES ${TSUGUMI_GEN_DIR}/bfbs/agent.bfbs ${TSUGUMI_GEN_DIR}/bfbs/host.bfbs ${TSUGUMI_GEN_DIR}/bfbs/tsugumi.bfbs
  DESTINATION share/tsugumi/protocol)

# CI builds only its external dependency double and public RPC scenarios
# against these existing archives. The normal runtime contains no test code.
set(TSUGUMI_TEST_SUPPORT_DIR "" CACHE PATH "Install reusable core test build inputs")
if(TSUGUMI_TEST_SUPPORT_DIR)
  install(TARGETS tsugumi_core tsugumi_agent_core tsugumi_json tsugumi_workerd_support tsugumi_worker_protocol tsugumi_peer_tls
    ARCHIVE DESTINATION ${TSUGUMI_TEST_SUPPORT_DIR}/lib)
  install(DIRECTORY include/ DESTINATION ${TSUGUMI_TEST_SUPPORT_DIR}/include)
  install(DIRECTORY ${PROJECT_SOURCE_DIR}/workerd/include/ DESTINATION ${TSUGUMI_TEST_SUPPORT_DIR}/include)
  install(FILES ${PROJECT_SOURCE_DIR}/workerd/src/journal.hh DESTINATION ${TSUGUMI_TEST_SUPPORT_DIR}/include/tsugumi-workerd)
  install(DIRECTORY src/ DESTINATION ${TSUGUMI_TEST_SUPPORT_DIR}/include/tsugumi-core
    FILES_MATCHING PATTERN "*.hh")
  install(FILES src/main.cc DESTINATION ${TSUGUMI_TEST_SUPPORT_DIR}/src)
  install(DIRECTORY ${TSUGUMI_GEN_DIR}/cpp/ DESTINATION ${TSUGUMI_TEST_SUPPORT_DIR}/include)
  install(DIRECTORY ${TSUGUMI_GEN_DIR}/python/ DESTINATION ${TSUGUMI_TEST_SUPPORT_DIR}/python)
endif()

# Supervisor of the shared VM (Linux guest only): one tsugumid per project.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(tsugumi-vmd src/vmd_main.cc)
  target_include_directories(tsugumi-vmd PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(tsugumi-vmd PRIVATE tsugumi_core)
  install(TARGETS tsugumi-vmd RUNTIME DESTINATION bin)
endif()
