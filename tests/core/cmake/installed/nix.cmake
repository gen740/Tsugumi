add_library(tsugumi_nix_eval STATIC IMPORTED)
set_target_properties(tsugumi_nix_eval PROPERTIES
  IMPORTED_LOCATION "${TSUGUMI_CORE_TEST_SUPPORT}/lib/libtsugumi_nix_eval.a")
target_link_libraries(tsugumi_nix_eval INTERFACE "$<LINK_ONLY:PkgConfig::NIX>" "$<LINK_ONLY:Threads::Threads>")
