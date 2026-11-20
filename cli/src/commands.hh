// The command's tool subcommands: one per Agent tool of the catalog the MCP
// server offers (`tsugumi run status --run-id ...`), and `tsugumi call` for
// a raw Agent request. Both work as hostd's external principal.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tsugumi::cli {
/** Lists the tools (starting the host when none runs). */
auto PrintTools(const std::filesystem::path& executable) -> void;
/** Runs the tool the arguments name; nullopt when they name none. */
auto RunTool(const std::filesystem::path& executable, const std::vector<std::string>& args) -> std::optional<int>;
auto RunCall(const std::filesystem::path& executable, const std::vector<std::string>& args) -> int;
}  // namespace tsugumi::cli
