# Use the same layout beside installed programs and in the CMake build tree.
set(TSUGUMI_CARD_NIX_OUTPUT share/tsugumi/nix)
function(tsugumi_card_nix_files directory)
  set(destination "${TSUGUMI_CARD_NIX_OUTPUT}")
  if(directory)
    string(APPEND destination "/${directory}")
  endif()
  foreach(path IN LISTS ARGN)
    get_filename_component(name "${path}" NAME)
    configure_file("${path}" "${PROJECT_BINARY_DIR}/${destination}/${name}" COPYONLY)
    install(FILES "${path}" DESTINATION "${destination}")
  endforeach()
endfunction()

tsugumi_card_nix_files("" nix/card-entry.nix)
# NIXPATH-CONTRACT: the evaluator directory carries only core-owned Nix files.
