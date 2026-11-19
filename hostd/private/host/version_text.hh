#pragma once

#include <string>

#include "tsugumi/protocol/host_generated.h"

namespace tsugumi::host {
auto CommitText(const Tsugumi::RPC::VersionCommitFactsT& facts, bool english)
    -> std::string;
auto RunText(const Tsugumi::RPC::VersionRunFactsT& facts, bool english)
    -> std::string;
auto ChangeText(const Tsugumi::RPC::VersionChangesT& facts, bool english)
    -> std::string;
auto PresentGraph(Tsugumi::RPC::VersionGraphT& graph, bool english) -> void;
}  // namespace tsugumi::host
