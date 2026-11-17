#pragma once

#include <filesystem>
#include <string>

#include "tsugumi/protocol/host_generated.h"

namespace tsugumi::host {
auto CommitText(const Tsugumi::RPC::VersionCommitFactsT& facts, bool english) -> std::string;
auto RunText(const Tsugumi::RPC::VersionRunFactsT& facts, bool english) -> std::string;
auto ChangeText(const Tsugumi::RPC::VersionChangesT& facts, bool english) -> std::string;
auto PresentGraph(Tsugumi::RPC::VersionGraphT& graph, bool english) -> void;
// Model-read policy belongs here. The project repository remains unchanged.
auto ResolveVersion(const std::filesystem::path &root, const std::string &name)
    -> std::string;
auto SummaryReads(const std::string& path) -> bool;
auto VersionDiff(const std::filesystem::path& root, const std::string& commit, const std::string& from = "")
    -> Tsugumi::Host::VersionDiffT;
auto ReadSummary(const std::filesystem::path& root, const std::string& commit, bool english) -> std::string;
auto StoreSummary(const std::filesystem::path& root, const std::string& commit, bool english,
                  const std::string& text) -> void;
}  // namespace tsugumi::host
