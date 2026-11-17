set(_tsugumi_host_layout_sources
  src/host/layout.cc
  src/host/layout_place.cc
  src/host/layout_wires.cc
  src/host/layout_service.cc
  src/host/layout_saving.cc
)

set_source_files_properties(src/host/layout.cc src/host/layout_place.cc src/host/layout_wires.cc
  PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
