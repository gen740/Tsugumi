#pragma once
#include <filesystem>
#include <string_view>
namespace tsugumi::workerd {
auto Authorized(const std::filesystem::path& file, std::string_view key) -> bool;
}  // namespace tsugumi::workerd
