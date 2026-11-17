add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/host_bfbs.cc
  COMMAND ${CMAKE_COMMAND} -DINPUT=${TSUGUMI_GEN_DIR}/bfbs/host.bfbs
    -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/host_bfbs.cc -DSYMBOL=tsugumi_host_bfbs
    -P ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
  DEPENDS ${TSUGUMI_GEN_DIR}/bfbs/host.bfbs VERBATIM)

include(${CMAKE_CURRENT_LIST_DIR}/agent-resources.cmake)

set(_template_files "")
foreach(name basic visualization reports csharp marimo go rust)
  list(APPEND _template_files ${PROJECT_SOURCE_DIR}/ui/templates/${name}.json)
endforeach()
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/host_templates.cc
  COMMAND ${CMAKE_COMMAND} -DINPUT_DIR=${PROJECT_SOURCE_DIR}/ui/templates
    -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/host_templates.cc
    -P ${PROJECT_SOURCE_DIR}/cmake/embed_templates.cmake
  DEPENDS ${_template_files} ${PROJECT_SOURCE_DIR}/cmake/embed_templates.cmake
    ${PROJECT_SOURCE_DIR}/cmake/embed.cmake VERBATIM)
include(${CMAKE_CURRENT_LIST_DIR}/host-agent-sources.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/host-util-sources.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/host-transport-sources.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/host-versions-sources.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/host-settings-sources.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/host-environment-sources.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/host-layout-sources.cmake)

add_library(tsugumi_host_core STATIC
  ${CMAKE_CURRENT_BINARY_DIR}/host_bfbs.cc
  ${_tsugumi_host_agent_sources}
  ${_tsugumi_host_transport_sources}
  ${_tsugumi_host_util_sources}
  src/host/access.cc
  src/host/state_format.cc
  src/host/state_gate.cc
  src/host/store.cc
  ${_tsugumi_host_settings_sources}
  src/host/demo_references.cc
  src/host/agent_exclude.cc
  src/host/registry.cc
  src/host/attached_folder.cc
  src/host/input_types.cc
  src/host/format.cc
  ${_tsugumi_host_versions_sources}
  src/host/comments.cc
  src/host/save.cc
  src/host/file_miss.cc
  src/host/project_edit.cc
  src/host/project_operations.cc
  src/host/document_edits.cc
  src/host/templates.cc
  ${CMAKE_CURRENT_BINARY_DIR}/host_templates.cc
  src/host/permission_kinds.cc
  src/host/guide.cc
  src/host/service.cc
  src/host/events.cc
  src/host/models.cc
  src/host/skills_service.cc
  src/host/skills.cc
  ${_tsugumi_host_layout_sources}
  src/host/projects.cc
  ${_tsugumi_host_environment_sources}
)
# Absolute paths to the pinned host tools. Empty means unavailable; never search PATH.
foreach(tool CLANG RUFF NIX SH)
  set(TSUGUMI_FORMAT_${tool} "" CACHE STRING "Host formatter ${tool}: absolute path or @bundle/name on macOS")
  target_compile_definitions(tsugumi_host_core PRIVATE TSUGUMI_FORMAT_${tool}="${TSUGUMI_FORMAT_${tool}}")
endforeach()
if(APPLE)
  target_link_libraries(tsugumi_host_core PRIVATE "-framework CoreFoundation")
else()
  find_package(ICU REQUIRED COMPONENTS uc i18n)
  target_link_libraries(tsugumi_host_core PRIVATE ICU::uc ICU::i18n)
endif()
target_include_directories(tsugumi_host_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
file(GLOB _host_headers RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/src CONFIGURE_DEPENDS src/host/*.hh src/agent/*.hh)
tsugumi_owned_headers(tsugumi_host_core host ${_host_headers} server.hh)
target_compile_options(tsugumi_host_core PRIVATE -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough)
set(TSUGUMI_HOST_SQLITE_LIBRARY "" CACHE FILEPATH "SQLite library used by the native host service")
target_link_libraries(tsugumi_host_core PUBLIC tsugumi_protocol tsugumi_json flatbuffers::flatbuffers
  Threads::Threads tsugumi_transport tsugumi_document tsugumi_sdk_describe tsugumi_client PkgConfig::LIBGIT2 CURL::libcurl tsugumi_worker_protocol tsugumi_peer_tls)
if(TSUGUMI_HOST_SQLITE_LIBRARY)
  target_link_libraries(tsugumi_host_core PRIVATE ${TSUGUMI_HOST_SQLITE_LIBRARY})
else()
  target_link_libraries(tsugumi_host_core PRIVATE PkgConfig::SQLITE3)
endif()
if(NOT APPLE)
  target_link_libraries(tsugumi_host_core PRIVATE PkgConfig::CRYPTO)
endif()
add_executable(tsugumi-hostd src/host/main.cc)
target_link_libraries(tsugumi-hostd PRIVATE tsugumi_host_core)
install(TARGETS tsugumi-hostd RUNTIME DESTINATION bin)
install(FILES ${TSUGUMI_GEN_DIR}/bfbs/agent.bfbs ${TSUGUMI_GEN_DIR}/bfbs/host.bfbs ${TSUGUMI_GEN_DIR}/bfbs/tsugumi.bfbs
  DESTINATION share/tsugumi/protocol)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(tsugumi_host_core PUBLIC util)
endif()
