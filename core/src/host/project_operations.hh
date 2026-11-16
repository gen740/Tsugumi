#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "host/project_edit.hh"

namespace tsugumi::host {
struct OperationReply {
  std::string json;
  std::string wire;
};
using OperationCall = std::function<OperationReply(
    const std::string&, const agent::json::Members&)>;
struct OperationContext {
  std::string origin;
  std::function<void(const std::string&, const SaveBase&)> authorize_file;
  // Recheck grants, aliases and the complete implicit write footprint against
  // this pinned state immediately before submitting the concrete plan.
  std::function<void(agent::json::Members&, const SaveBase&)> authorize;
};
class ProjectOperations {
 public:
  auto Queue(const std::string& project)
      -> std::shared_ptr<std::recursive_mutex>;
  static auto Digest(const Tsugumi::Host::ProjectEditsT& edits,
                     const std::string& origin) -> std::string;
  auto Execute(const std::string& project,
               const Tsugumi::RPC::OperationSubmissionT& operation,
               const Tsugumi::Host::ProjectEditsT& edits,
               const OperationContext& context, const OperationCall& call)
      -> OperationReply;

 private:
  std::mutex mutex_;
  std::map<std::string, std::weak_ptr<std::recursive_mutex>> queues_;
};
}  // namespace tsugumi::host
