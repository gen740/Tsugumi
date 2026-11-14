// The single module source delivered over RPC to every replaceable UI.
#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace tsugumi {
inline constexpr size_t kModuleSourceLimit = 32u << 20;
auto valid_module_identity(std::string_view identity) -> bool;
auto wasm_module_source(std::string_view entry, std::string_view wasm) -> std::string;
// store_directory maps canonical store identities into a worker's private store.
auto module_source(const std::filesystem::path& root,
                   const std::filesystem::path& store_directory = "/nix/store") -> std::string;
auto cached_module_source(const std::filesystem::path& cache, std::string_view identity) -> std::optional<std::string>;
auto save_module_source(const std::filesystem::path& cache, std::string_view identity, std::string_view source) -> void;
}  // namespace tsugumi
