#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include "host/comments.hh"
#include "host/layout_saving.hh"
#include "host/project_edit.hh"
#include "host/templates.hh"

namespace tsugumi::host {
struct OperationReply {
  std::string json;
  std::string wire;
};
using OperationCall = std::function<OperationReply(
    const std::string&, const ::tsugumi::json::Members&)>;
struct SavePreparation {
  bool stamp_agent = false;
  comments::ReserveComment reserve_comment;
  std::function<void(::tsugumi::json::Members&)> finish;
};
struct SubmittedSave {
  OperationReply reply;
  std::string comments;
};
auto SubmitProjectSave(::tsugumi::json::Members plan, const SaveBase& base,
                       const SourcePatches& patches, const OperationCall& call,
                       const SavePreparation& preparation) -> SubmittedSave;

struct OperationContext {
  std::string origin;
  std::function<void(const std::string&, const SaveBase&)> authorize_file;
  std::function<void(::tsugumi::json::Members&, const SaveBase&)> authorize;
  std::function<std::filesystem::path(const std::string&)> demo = {};
  std::filesystem::path host_templates = {};
  std::function<DocumentTemplate(const Tsugumi::Host::TemplateAddT&)>
      read_template_source = {};
  std::function<std::string(const std::string&,
                            const std::function<std::string()>&)>
      reserve_template_id = {};
};
class ProjectOperations {
 public:
  auto Execute(const Tsugumi::Host::ProjectOperateRequestT& operation,
               const OperationContext& context, const OperationCall& call)
      -> OperationReply;
};
}  // namespace tsugumi::host
