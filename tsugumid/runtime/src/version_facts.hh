// Translation from private persisted version records to public typed facts.
#pragma once
#include <memory>
#include <string>
#include "tsugumi/protocol/tsugumi_generated.h"
namespace tsugumi::version_facts {
auto commit(const std::string& raw) -> std::unique_ptr<Tsugumi::RPC::VersionCommitFactsT>;
auto run(const std::string& raw) -> std::unique_ptr<Tsugumi::RPC::VersionRunFactsT>;
auto changes(const std::string& raw) -> Tsugumi::RPC::VersionChangesT;
}  // namespace tsugumi::version_facts
