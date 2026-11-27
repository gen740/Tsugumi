add_library(tsugumi_nix_store STATIC src/store.cc src/derivation_transport.cc
  src/cache_relay_store.cc)
target_compile_features(tsugumi_nix_store PRIVATE cxx_std_23)
target_include_directories(tsugumi_nix_store PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tsugumi_nix_store PRIVATE PkgConfig::NIX Threads::Threads)
