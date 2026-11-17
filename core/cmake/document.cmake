# Document loading and resolution are also used by the host before a project
# daemon exists. Keep evaluation, execution and project storage out of that link.
add_library(tsugumi_document STATIC
  src/types.cc
  src/document.cc
  src/graph.cc
  src/worker_condition.cc
  src/input_types.cc
  src/nix_refs.cc
  src/collect_type.cc
  src/host_requirements.cc)
target_include_directories(tsugumi_document PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(tsugumi_document PUBLIC tsugumi_protocol tsugumi_json
  flatbuffers::flatbuffers Threads::Threads tsugumi_workerd_support)

set_source_files_properties(src/worker_condition.cc PROPERTIES
  COMPILE_OPTIONS "-Wall;-Wextra;-Werror;-Wformat=2;-Wimplicit-fallthrough")

target_compile_definitions(tsugumi_document PRIVATE
  TSUGUMI_CARD_NIXPKGS_REV="${TSUGUMI_CARD_NIXPKGS_REV}"
  TSUGUMI_CARD_NIXPKGS_NARHASH="${TSUGUMI_CARD_NIXPKGS_NARHASH}")
