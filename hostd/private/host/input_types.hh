#pragma once

#include <optional>
#include <string>

#include "tsugumi/protocol/host_generated.h"

namespace tsugumi::host {
// Language presentation of neutral schemas returned by the project RPC.
auto InputTypes(const Tsugumi::RPC::InputTypesResult& resolved,
                const std::optional<std::string>& language)
    -> Tsugumi::Host::InputTypesT;
}  // namespace tsugumi::host
