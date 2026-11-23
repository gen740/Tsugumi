# NIXPATH-CONTRACT: link the literal parser into the evaluator.
add_library(tsugumi_nix_eval STATIC runtime/src/nix_eval.cc runtime/src/nix_params.cc)
target_compile_features(tsugumi_nix_eval PRIVATE cxx_std_23)
target_include_directories(tsugumi_nix_eval PRIVATE ${PROJECT_SOURCE_DIR}/workerd/include)
target_link_libraries(tsugumi_nix_eval PUBLIC tsugumi_nix_store PRIVATE tsugumi_document tsugumi_protocol flatbuffers::flatbuffers tsugumi_util PkgConfig::NIX Threads::Threads)
