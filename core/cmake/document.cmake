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
target_include_directories(tsugumi_document PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
tsugumi_owned_headers(tsugumi_document document schema.hh document.hh graph.hh definition_values.hh types.hh worker_condition.hh input_types.hh nix_refs.hh collect_type.hh host_requirements.hh)
target_link_libraries(tsugumi_document PUBLIC tsugumi_protocol tsugumi_json
  flatbuffers::flatbuffers Threads::Threads tsugumi_util)

set_source_files_properties(src/worker_condition.cc PROPERTIES
  COMPILE_OPTIONS "-Wall;-Wextra;-Werror;-Wformat=2;-Wimplicit-fallthrough")

target_compile_definitions(tsugumi_document PRIVATE
  TSUGUMI_CARD_NIXPKGS_REV="${TSUGUMI_CARD_NIXPKGS_REV}"
  TSUGUMI_CARD_NIXPKGS_NARHASH="${TSUGUMI_CARD_NIXPKGS_NARHASH}")
