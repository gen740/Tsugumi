// The Claude Code plugin's MCP server (`tsugumi mcp`).
#pragma once

#include <filesystem>

namespace tsugumi::cli {
auto RunMcp(const std::filesystem::path& executable) -> int;
}  // namespace tsugumi::cli
