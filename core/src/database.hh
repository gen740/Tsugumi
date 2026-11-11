// Logger database sinks. Connection URLs are public; credentials are
// host-owned.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "graph.hh"

namespace tsugumi::database {
auto valid_table(std::string_view name) -> bool;
auto validate_url(const std::string& url) -> void;
auto write(const std::filesystem::path& project,
           const std::filesystem::path& credentials, const Node& node,
           const std::string& run, uint64_t invocation,
           const std::string& value, uint64_t timestamp_ms) -> void;
}  // namespace tsugumi::database
