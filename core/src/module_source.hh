// The single module source delivered over RPC to every replaceable UI.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace tsugumi {
auto valid_module_identity(std::string_view identity) -> bool;
auto wasm_module_source(std::string_view entry, std::string_view wasm) -> std::string;
auto module_source(const std::filesystem::path& root) -> std::string;
}  // namespace tsugumi
