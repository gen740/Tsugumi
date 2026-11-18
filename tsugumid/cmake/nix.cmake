add_library(tsugumi_nix_eval STATIC runtime/src/nix_eval.cc)
target_compile_features(tsugumi_nix_eval PRIVATE cxx_std_23)
target_link_libraries(tsugumi_nix_eval PUBLIC tsugumi_nix_store PRIVATE PkgConfig::NIX Threads::Threads)
