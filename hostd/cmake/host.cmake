include(${CMAKE_CURRENT_LIST_DIR}/agent-resources.cmake)

set(_template_files "")
foreach(name basic input/Sources/InputTemplates/inputs visualization reports csharp marimo go rust)
  list(APPEND _template_files ${PROJECT_SOURCE_DIR}/ui/shared/templates/${name}.json)
endforeach()
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/host_templates.cc
  COMMAND ${CMAKE_COMMAND} -DINPUT_DIR=${PROJECT_SOURCE_DIR}/ui/shared/templates
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
  ${_tsugumi_host_agent_sources}
  ${_tsugumi_host_transport_sources}
  ${_tsugumi_host_util_sources}
  ${PROJECT_SOURCE_DIR}/hostd/src/access.cc
  ${PROJECT_SOURCE_DIR}/hostd/src/store.cc
  ${_tsugumi_host_settings_sources}
  ${PROJECT_SOURCE_DIR}/hostd/src/demo_references.cc
  ${PROJECT_SOURCE_DIR}/hostd/src/agent_exclude.cc
  ${PROJECT_SOURCE_DIR}/hostd/src/registry.cc
  ${PROJECT_SOURCE_DIR}/hostd/src/attached_folder.cc
  ${PROJECT_SOURCE_DIR}/hostd/src/input_types.cc
  ${PROJECT_SOURCE_DIR}/hostd/src/format.cc
  ${_tsugumi_host_versions_sources}
  ${PROJECT_SOURCE_DIR}/hostd/src/comments.cc
  ${PROJECT_SOURCE_DIR}/hostd/src/save.cc
  ${PROJECT_SOURCE_DIR}/hostd/src/file_miss.cc
  ${PROJECT_SOURCE_DIR}/hostd/src/project_edit.cc
  ${PROJECT_SOURCE_DIR}/hostd/src/project_operations.cc
  ${PROJECT_SOURCE_DIR}/hostd/src/document_edits.cc
  ${PROJECT_SOURCE_DIR}/hostd/src/templates.cc
  ${CMAKE_CURRENT_BINARY_DIR}/host_templates.cc
  ${PROJECT_SOURCE_DIR}/hostd/src/permission_kinds.cc
  ${PROJECT_SOURCE_DIR}/hostd/src/guide.cc
  ${PROJECT_SOURCE_DIR}/hostd/src/service.cc
  ${PROJECT_SOURCE_DIR}/hostd/src/events.cc
  ${PROJECT_SOURCE_DIR}/hostd/src/models.cc
  ${PROJECT_SOURCE_DIR}/hostd/src/skills_service.cc
  ${PROJECT_SOURCE_DIR}/hostd/src/skills.cc
  ${_tsugumi_host_layout_sources}
  ${PROJECT_SOURCE_DIR}/hostd/src/projects.cc
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
target_include_directories(tsugumi_host_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/private ${CMAKE_CURRENT_SOURCE_DIR}/agent/private)
target_compile_options(tsugumi_host_core PRIVATE -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough)
target_link_libraries(tsugumi_host_core PUBLIC tsugumi_trace_codec tsugumi_rpc_schema tsugumi_protocol tsugumi_json flatbuffers::flatbuffers
  Threads::Threads tsugumi_transport tsugumi_document tsugumi_client CURL::libcurl tsugumi_worker_protocol tsugumi_peer_tls tsugumi_launch_paths)
target_link_libraries(tsugumi_host_core PRIVATE native_sqlite)
if(NOT APPLE)
  target_link_libraries(tsugumi_host_core PRIVATE PkgConfig::CRYPTO)
endif()
add_executable(tsugumi-hostd ${PROJECT_SOURCE_DIR}/hostd/src/main.cc)
target_include_directories(tsugumi-hostd PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/private)
target_link_libraries(tsugumi-hostd PRIVATE tsugumi_host_core)
install(TARGETS tsugumi-hostd RUNTIME DESTINATION bin)
install(FILES ${TSUGUMI_GEN_DIR}/bfbs/agent.bfbs ${TSUGUMI_GEN_DIR}/bfbs/host.bfbs ${TSUGUMI_GEN_DIR}/bfbs/tsugumi.bfbs
  DESTINATION share/tsugumi/protocol)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(tsugumi_host_core PUBLIC util)
endif()
