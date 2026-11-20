# Document loading and resolution are also used by the host before a project
# daemon exists. Keep evaluation, execution and project storage out of that link.
add_library(tsugumi_document STATIC
  ${PROJECT_SOURCE_DIR}/tsugumid/document/src/comments.cc
  ${PROJECT_SOURCE_DIR}/tsugumid/document/src/types.cc
  ${PROJECT_SOURCE_DIR}/tsugumid/document/src/document.cc
  ${PROJECT_SOURCE_DIR}/tsugumid/document/src/graph.cc
  ${PROJECT_SOURCE_DIR}/tsugumid/document/src/worker_condition.cc
  ${PROJECT_SOURCE_DIR}/tsugumid/document/src/input_types.cc
  ${PROJECT_SOURCE_DIR}/tsugumid/document/src/nix_refs.cc
  ${PROJECT_SOURCE_DIR}/tsugumid/document/src/collect_type.cc
  ${PROJECT_SOURCE_DIR}/tsugumid/document/src/host_requirements.cc)
target_include_directories(tsugumi_document PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tsugumi_document PUBLIC tsugumi_protocol tsugumi_json
  flatbuffers::flatbuffers Threads::Threads tsugumi_util)

set_source_files_properties(${PROJECT_SOURCE_DIR}/tsugumid/document/src/worker_condition.cc PROPERTIES
  COMPILE_OPTIONS "-Wall;-Wextra;-Werror;-Wformat=2;-Wimplicit-fallthrough")

target_compile_definitions(tsugumi_document PRIVATE
  TSUGUMI_CARD_NIXPKGS_REV="${TSUGUMI_CARD_NIXPKGS_REV}"
  TSUGUMI_CARD_NIXPKGS_NARHASH="${TSUGUMI_CARD_NIXPKGS_NARHASH}")
