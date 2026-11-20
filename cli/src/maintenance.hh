// `tsugumi update` and `tsugumi uninstall`, for the installer's layout
// (cli/install.sh).
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace tsugumi::cli {
auto Update(const std::filesystem::path& root) -> int;
auto Uninstall(const std::vector<std::string>& args) -> int;
}  // namespace tsugumi::cli
