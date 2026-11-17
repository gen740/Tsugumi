# Phase A keeps source paths. Publish only each component's owned headers;
# private source roots are never inherited by consumers.
function(tsugumi_owned_headers target owner)
  set(destination "${PROJECT_BINARY_DIR}/headers/${owner}")
  foreach(header IN LISTS ARGN)
    get_filename_component(parent "${destination}/${header}" DIRECTORY)
    file(MAKE_DIRECTORY "${parent}")
    configure_file("${PROJECT_SOURCE_DIR}/core/src/${header}" "${destination}/${header}" COPYONLY)
  endforeach()
  target_include_directories(${target} PUBLIC "${destination}")
endfunction()
