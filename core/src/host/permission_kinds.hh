#pragma once

#include <string>

#include "tsugumi/protocol/host_generated.h"

namespace tsugumi::host {
auto PermissionKinds(const std::string& language)
    -> Tsugumi::Host::PermissionKindsT;
}  // namespace tsugumi::host
