#pragma once

#include <string>

namespace tsugumi::sdk {
// A reading view of the SDK's generated input. Runs the same pinned flatc
// generators in memory; the output contains declarations, never adapters.
auto DescribeInput(const std::string& schema, const std::string& language)
    -> std::string;
}  // namespace tsugumi::sdk
