#pragma once
#include "tsugumi/workerd/module_source.hh"
namespace tsugumi {
auto cached_module_source(const std::filesystem::path& cache, std::string_view identity) -> std::optional<std::string>;
auto save_module_source(const std::filesystem::path& cache, std::string_view identity, std::string_view source) -> void;
}  // namespace tsugumi
