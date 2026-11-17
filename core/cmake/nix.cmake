# Nix integration units use C++23; the rest of the core never includes Nix headers.
add_library(tsugumi_nix_eval STATIC src/nix_eval.cc src/derivation_transport.cc)
target_compile_features(tsugumi_nix_eval PRIVATE cxx_std_23)
target_link_libraries(tsugumi_nix_eval PRIVATE PkgConfig::NIX Threads::Threads)
