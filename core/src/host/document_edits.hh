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

auto CopyDocuments(const std::string &state,
                   const std::vector<std::string> &paths,
                   const std::string &from, const std::string &to,
                   const std::string &name) -> Tsugumi::Host::DocumentCopyT;

auto ConnectDocuments(const std::string &state, const std::string &from_card,
                      const std::string &from_port, const std::string &to_card,
                      const std::string &to_port, const std::string &fork_id)
    -> Tsugumi::Host::DocumentConnectT;

auto DocumentsEqual(const std::string& a, const std::string& b) -> bool;

} // namespace tsugumi::host
