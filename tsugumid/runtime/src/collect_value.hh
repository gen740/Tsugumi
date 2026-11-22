// Collect Card values into the fixed Collected table, preserving each name,
// kind and opaque bytes or path.
#pragma once

#include <atomic>
#include <span>

#include "tsugumi/document/types.hh"
#include "tsugumi/workerd/values.hh"

namespace tsugumi {

auto collect_values(const types::PortType& output,
                    std::span<const Value> members,
                    const std::atomic<bool>& cancel) -> Value;

}  // namespace tsugumi
