#include <algorithm>

#include "agent/tools.hh"
#include "host/failure.hh"
#include "host/layout_saving.hh"

namespace tsugumi::agent {
namespace {
namespace H = Tsugumi::Host;
using host::Failure;
auto Edits(const json::Members& arguments) -> H::ProjectEditsT {
  H::ProjectEditsT result;
  for (const auto& raw :
       json::Array(json::Get(arguments, "edits", "[]"), "edits")) {
    const auto at = "edits[" + std::to_string(result.edits.size()) + "]";
    const auto args = json::Object(raw, at);
    const auto path = json::Text(args, "file_path");
    auto edit = std::make_unique<H::ProjectEditT>();
    if (args.contains("selector")) {
      H::ProjectFieldEditT field;
      field.file_path = path;
      for (const auto& raw_step :
           json::Array(args.at("selector"), at + ".selector")) {
        const auto step_at =
            at + ".selector[" + std::to_string(field.selector.size()) + "]";
        const auto step = json::Object(raw_step, step_at);
        auto selector = std::make_unique<H::ProjectSelectorT>();
        selector->field = json::Text(step, "field");
        selector->match_json = json::Get(step, "match", "");
        field.selector.push_back(std::move(selector));
      }
      auto value = [&](const char* key) {
        const auto raw_value =
            json::Object(json::Get(args, key, "{}"), at + "." + key);
        if (!raw_value.contains("present"))
          throw Failure("invalid_request",
                        "value requires an explicit presence discriminator");
        auto out = std::make_unique<H::ProjectValueT>();
        out->present = json::Bool(raw_value.at("present"));
        if (out->present && !raw_value.contains("value"))
          throw Failure("invalid_request", "present value is missing");
        out->json = json::Get(raw_value, "value", "");
        return out;
      };
      field.old_value = value("old_value");
      field.new_value = value("new_value");
      edit->value.Set(std::move(field));
    } else if (args.contains("old_string")) {
      H::ProjectTextEditT text;
      text.file_path = path;
      text.old_string = json::Text(args, "old_string");
      text.new_string = json::Text(args, "new_string");
      const auto count =
          json::UInt(json::Get(args, "expected_matches", "1"), UINT64_MAX);
      if (count > UINT32_MAX)
        throw Failure("invalid_request", "invalid expected_matches");
      text.expected_matches = static_cast<uint32_t>(count);
      edit->value.Set(std::move(text));
    } else if (args.contains("put")) {
      H::ProjectFilePutT put;
      put.file_path = path;
      put.expected_hash = json::Text(args, "expected_hash");
      put.expect_missing = json::Bool(json::Get(args, "expect_missing"));
      const auto bytes = json::Text(args, "put");
      put.bytes.assign(bytes.begin(), bytes.end());
      edit->value.Set(std::move(put));
    } else if (args.contains("remove")) {
      if (!json::Bool(args.at("remove")))
        throw Failure("invalid_request", "remove must be true");
      H::ProjectFileRemoveT remove;
      remove.file_path = path;
      remove.expected_hash = json::Text(args, "expected_hash");
      edit->value.Set(std::move(remove));
    } else if (args.contains("move_to")) {
      H::ProjectFileMoveT move;
      move.file_path = path;
      move.expected_hash = json::Text(args, "expected_hash");
      move.to = json::Text(args, "move_to");
      move.expected_destination_hash =
          json::Text(args, "expected_destination_hash");
      move.destination_missing =
          json::Bool(json::Get(args, "destination_missing"));
      edit->value.Set(std::move(move));
    } else
      throw Failure("invalid_request", "missing project_edit variant");
    result.edits.push_back(std::move(edit));
  }
  return result;
}
auto Outcome(const std::string& response) -> json::Members {
  const auto reply = json::Object(response);
  const auto value = json::Object(json::Get(reply, "value", "{}"));
  if (json::Text(reply, "reply") == "Failure")
    throw Failure(json::Text(value, "kind"), json::Text(value, "diagnostic"));
  return value;
}
}  // namespace

auto Tools::EditProjectTool(const std::string& conversation,
                            const std::string& project, const ToolCall& call,
                            const json::Members& args,
                            const std::string& operation, HelperScope* boundary,
                            bool recovering, std::stop_token stop)
    -> std::string {
  if (recovering) {
    const auto status = Outcome(RawProjectCall(
        project, "operation_get",
        json::Encode(json::Members{{"op_id", json::Quote(operation)}}), stop));
    if (json::Text(status, "state") != "completed")
      throw Failure("outcome_unknown",
                    "The earlier operation has no terminal receipt");
    const auto receipt = json::Object(json::Get(status, "receipt"));
    return json::Encode(
        json::Members{{"reply", json::Get(receipt, "result_type")},
                      {"value", json::Get(receipt, "result", "{}")}});
  }
  auto refused = [](const std::string& kind, const std::string& message) {
    return json::Encode(json::Members{
        {"reply", json::Quote("Failure")},
        {"value",
         json::Encode(json::Members{{"kind", json::Quote(kind)},
                                    {"message", json::Quote(message)},
                                    {"diagnostic", json::Quote(message)},
                                    {"args", "[]"}})}});
  };
  try {
    H::ProjectOperateRequestT request;
    request.project_id = project;
    request.op_id = operation;
    const auto store = json::Text(args, "store");
    auto selected_store = [&] {
      if (store == "project") return H::TemplateStore::Project;
      if (store == "host") return H::TemplateStore::Host;
      throw Failure("invalid_request",
                    "Choose the project or host template store");
    };
    const bool adding =
        call.name == "template_add" || call.name == "template_import";
    if (call.name == "nix_path_lock_update") {
      H::ProjectUpdateNixPathLockT value;
      value.project_path = json::Text(args, "project_path");
      value.name = json::Text(args, "name");
      request.body.Set(std::move(value));
    } else if (adding) {
      H::TemplateAddT value;
      value.name = json::Text(args, "name");
      if (!host::TemplateName(value.name))
        throw Failure("invalid_request", "Choose a valid template name");
      if (call.name == "template_add") {
        H::TemplateFolderT folder;
        folder.path = json::Text(args, "path");
        value.source.Set(std::move(folder));
      } else {
        H::TemplateGitT git;
        git.url = json::Text(args, "url");
        git.ref = json::Text(args, "ref");
        git.subdirectory = json::Text(args, "subdirectory");
        value.source.Set(std::move(git));
      }
      if (selected_store() == H::TemplateStore::Host) {
        const auto policy = Exclusions(project, stop);
        policy.Check(dependencies.host_templates / value.name);
        AddHostTemplate(value, stop,
                        [&](const auto& path) { policy.Check(path); });
        return R"({"reply":"Acknowledgement","value":{}})";
      }
      request.body.Set(std::move(value));
    } else if (call.name == "template_copy") {
      H::ProjectTemplateCopyT value;
      value.project_path = json::Text(args, "project_path");
      value.name = json::Text(args, "name");
      value.store = selected_store();
      auto coordinate = [&](const char* field) {
        const auto raw = json::Get(args, field, "0");
        size_t end = 0;
        const auto number = std::stod(raw, &end);
        if (end != raw.size())
          throw Failure("invalid_request", "Invalid template position");
        return number;
      };
      value.x = coordinate("x");
      value.y = coordinate("y");
      request.body.Set(std::move(value));
    } else if (call.name == "project_edit")
      request.body.Set(Edits(args));
    else if (call.name == "project_connect") {
      H::ProjectConnectT value;
      value.project_path = json::Text(args, "project_path");
      value.from_card = json::Text(args, "from_card");
      value.from_port = json::Text(args, "from_port");
      value.to_card = json::Text(args, "to_card");
      value.to_port = json::Text(args, "to_port");
      request.body.Set(std::move(value));
    } else if (call.name == "project_make_subgraph") {
      H::ProjectMakeSubgraphT value;
      value.project_path = json::Text(args, "project_path");
      value.id = json::Text(args, "id");
      for (const auto& raw :
           json::Array(json::Get(args, "cards", "[]"), "cards"))
        value.cards.push_back(json::String(raw));
      request.body.Set(std::move(value));
    } else {
      H::ProjectLayoutT value;
      value.project_path = json::Text(args, "project_path");
      const auto action = json::Text(args, "action");
      if (action != "arrange" && action != "untangle")
        throw Failure("invalid_request", "unknown layout action");
      value.action = action == "arrange" ? H::ProjectLayoutAction::Arrange
                                         : H::ProjectLayoutAction::Untangle;
      request.body.Set(std::move(value));
    }
    std::optional<host::AgentExclude> policy;
    HelperScope::Reader read;
    auto use_inspection = [&](const host::SaveBase& base) {
      if (policy) return;
      policy = ExclusionsFor(project, base);
      if (boundary && boundary->scoped())
        read = HelperReader(conversation, stop, {}, &base);
    };
    host::OperationContext context;
    context.origin = "agent";
    context.authorize_file = [&](const std::string& path,
                                 const host::SaveBase& base) {
      use_inspection(base);
      policy->Check(path);
      if (std::filesystem::path(path).is_absolute()) return;
      if (!boundary || !boundary->scoped()) return;
      // Project documents are read by the host only; the expanded graph diff is
      // checked below. Source and definition reads require the exact read set.
      if (path == "tsugumi.json" ||
          host::IsExternalDocument(path, "tsugumi.json"))
        return;
      boundary->Check("project_file_read", {{"path", json::Quote(path)}}, read);
    };
    if (adding)
      context.read_template_source = [&](const auto& input) {
        const auto excluded = Exclusions(project, stop);
        return host::ReadTemplateSource(
            input, stop, [&](const auto& path) { excluded.Check(path); });
      };
    context.authorize = [&](json::Members& plan, const host::SaveBase& base) {
      {
        std::lock_guard lock(mutex);
        if (boundary) RequireHelperPermission(*boundary);
      }
      use_inspection(base);
      if (read) boundary->CheckCurrentSave(plan, read);
      policy->CheckSave(plan, base, {}, adding);
      if (args.contains("approved_permissions"))
        plan["approved_permissions"] = args.at("approved_permissions");
    };
    return OperateProject(request, context, stop).json;
  } catch (const json::DecodeFailure& error) {
    return refused("invalid_request", error.what());
  } catch (const Cancelled&) {
    throw;
  } catch (const Failure& error) {
    if (!error.request_id.empty() || error.code == "outcome_unknown" ||
        error.code == "unavailable" || error.code == "busy" ||
        error.code == "internal_error")
      throw;
    // Preflight scope refusals also need a terminal receipt. ScopedResult
    // redacts the diagnostic when delivering it to a helper.
    return refused(error.code, error.what());
  } catch (const std::exception& error) {
    return refused("validation_failed", error.what());
  }
}

}  // namespace tsugumi::agent
