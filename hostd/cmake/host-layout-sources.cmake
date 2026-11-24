set(_tsugumi_host_layout_sources
  ${PROJECT_SOURCE_DIR}/hostd/src/layout.cc
  ${PROJECT_SOURCE_DIR}/hostd/src/layout_place.cc
  ${PROJECT_SOURCE_DIR}/hostd/src/layout_wires.cc
  ${PROJECT_SOURCE_DIR}/hostd/src/layout_service.cc
  ${PROJECT_SOURCE_DIR}/hostd/src/layout_saving.cc
  ${PROJECT_SOURCE_DIR}/hostd/src/layout_file.cc
)

set_source_files_properties(${PROJECT_SOURCE_DIR}/hostd/src/layout.cc ${PROJECT_SOURCE_DIR}/hostd/src/layout_place.cc ${PROJECT_SOURCE_DIR}/hostd/src/layout_wires.cc
  PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
