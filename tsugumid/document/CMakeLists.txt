# Document loading and resolution are also used by the host before a project
# daemon exists. Keep evaluation, execution and project storage out of that link.
# The document schemas, compiled by the FlatBuffers parser that reads the
# documents' JSON text.
set(_document_schema_sources "")
foreach(schema ${TSUGUMI_DOCUMENT_SCHEMAS})
  set(_output ${CMAKE_CURRENT_BINARY_DIR}/${schema}_fbs.cc)
  add_custom_command(
    OUTPUT ${_output}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${TSUGUMI_PROTOCOL_DIR}/${schema}.fbs
      -DOUTPUT=${_output} -DSYMBOL=tsugumi_${schema}_fbs
      -P ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS ${TSUGUMI_PROTOCOL_DIR}/${schema}.fbs ${PROJECT_SOURCE_DIR}/cmake/embed.cmake VERBATIM)
  list(APPEND _document_schema_sources ${_output})
endforeach()

add_library(tsugumi_document STATIC
  ${_document_schema_sources}
  ${PROJECT_SOURCE_DIR}/tsugumid/document/src/text.cc
  ${PROJECT_SOURCE_DIR}/tsugumid/document/src/readme.cc
  ${PROJECT_SOURCE_DIR}/tsugumid/document/src/comments.cc
  ${PROJECT_SOURCE_DIR}/tsugumid/document/src/types.cc
  ${PROJECT_SOURCE_DIR}/tsugumid/document/src/document.cc
  ${PROJECT_SOURCE_DIR}/tsugumid/document/src/graph.cc
  ${PROJECT_SOURCE_DIR}/tsugumid/document/src/worker_condition.cc
  ${PROJECT_SOURCE_DIR}/tsugumid/document/src/input_types.cc
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
