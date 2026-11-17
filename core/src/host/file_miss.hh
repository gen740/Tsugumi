#pragma once

#include "host/save.hh"

namespace tsugumi::host {
// Supplement a file miss from confirmed project content, never disk contents.
auto DescribeFileMiss(const std::string& path,
                      const agent::json::Members& snapshot,
                      const SaveRead& read) -> std::string;
}  // namespace tsugumi::host
