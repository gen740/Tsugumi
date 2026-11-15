// Output type resolution for Collect Cards, independent of run values.
#pragma once

#include <string>

#include "types.hh"

namespace tsugumi {

// The output message type of a Collect Card whose input port is `source`.
// Returns null with `error` set when the source type cannot be collected.
auto collect_output_type(const types::PortType& source, std::string& error) -> types::TypeRef;

}  // namespace tsugumi
