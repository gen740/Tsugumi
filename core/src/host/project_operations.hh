#pragma once

#include <functional>
#include <string>

#include "host/layout_saving.hh"
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
  std::function<void(agent::json::Members&, const SaveBase&)> authorize;
  std::function<LayoutWrite(const SaveBase&)> layout_scope;
};
class ProjectOperations {
 public:
  auto Execute(const std::string& project,
               const Tsugumi::Host::ProjectOperateRequestT& operation,
               const OperationContext& context, const OperationCall& call)
      -> OperationReply;
};
}  // namespace tsugumi::host
