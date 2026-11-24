// Collect opaque envelopes in arrival order into the fixed Collected table.
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
