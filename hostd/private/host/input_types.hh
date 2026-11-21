#pragma once

#include "tsugumi/protocol/host_generated.h"

namespace tsugumi::host {
// Render the resolved port types as complete FlatBuffers schemas.
auto InputTypes(const Tsugumi::RPC::InputTypesResult& resolved)
    -> Tsugumi::Host::InputTypesT;
}  // namespace tsugumi::host
