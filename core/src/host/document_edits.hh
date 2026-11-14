#pragma once

#include <string>
#include <vector>

#include "tsugumi/protocol/host_generated.h"

namespace tsugumi::host {

// Pure document edits. The caller saves the returned documents, definitions
// and file moves together through project_save; the host never writes them.
auto MakeSubgraph(const std::string &project,
                  const std::vector<std::string> &cards, const std::string &id)
    -> Tsugumi::Host::DocumentSubgraphT;

auto MergeDocuments(const std::string &base, const std::string &ours,
                    const std::string &theirs) -> Tsugumi::Host::DocumentMergeT;

} // namespace tsugumi::host
