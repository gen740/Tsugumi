// Output type resolution for Collect Cards, independent of run values.
#pragma once

#include <string_view>

#include <string>

#include "tsugumi/document/types.hh"

namespace tsugumi {

auto collect_schema_declarations() -> std::string_view;

// The fixed Collected table is the output for every member kind.
auto collect_output_type(std::string& error) -> types::TypeRef;

}  // namespace tsugumi
