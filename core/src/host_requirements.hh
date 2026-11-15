// Definition-derived host requirements. Policy belongs to the host service.
#pragma once

#include "graph.hh"
#include "tsugumi/protocol/tsugumi_generated.h"

namespace tsugumi {
auto host_requirements(const Resolution& resolution, uint64_t rev = 0)
    -> Tsugumi::RPC::ProjectHostRequirementsT;
// Bootstrap before the VM's project mount namespace exists. Uses the same
// document loader, source integrity checks and resolver as the project core.
auto load_host_requirements(const std::filesystem::path& root)
    -> Tsugumi::RPC::ProjectHostRequirementsT;
}  // namespace tsugumi
