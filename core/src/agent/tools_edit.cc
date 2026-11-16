#include <ctime>

#include "agent/tools.hh"
#include "host/access.hh"
#include "host/failure.hh"
#include "host/layout_saving.hh"
#include "util/crypto.hh"

namespace tsugumi::agent {
namespace {
namespace H = Tsugumi::Host;
namespace R = Tsugumi::RPC;
using host::Failure;
auto Edits(const json::Members& arguments) -> H::ProjectEditsT {
  H::ProjectEditsT result;
  for (const auto& raw : json::Array(json::Get(arguments, "edits", "[]"))) {
    const auto args = json::Object(raw);
    const auto path = json::Text(args, "file_path");
    auto edit = std::make_unique<H::ProjectEditT>();
    if (args.contains("selector")) {
      H::ProjectFieldEditT field;
      field.file_path = path;
      for (const auto& raw_step : json::Array(args.at("selector"))) {
        const auto step = json::Object(raw_step);
        auto selector = std::make_unique<H::ProjectSelectorT>();
        selector->field = json::Text(step, "field");
        selector->match_json = json::Get(step, "match", "");
        field.selector.push_back(std::move(selector));
      }
      auto value = [&](const char* key) {
        const auto raw_value = json::Object(json::Get(args, key, "{}"));
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
      if (count == 0 || count > UINT32_MAX)
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
auto Submission(const std::string& lane, const std::string& digest)
    -> std::string {
  return json::Encode(json::Members{
      {"identity", json::Encode(json::Members{{"lane", json::Quote(lane)},
                                              {"sequence", "\"1\""}})},
      {"semantic_digest", json::Quote(digest)}});
}
}  // namespace

auto Tools::EditProjectTool(const std::string& conversation,
                            const std::string& project, const ToolCall& call,
                            const json::Members& args,
                            const std::string& operation, HelperScope* boundary,
                            bool recovering, std::stop_token stop)
    -> std::string {
  const auto edits = Edits(args);
  const auto digest = host::ProjectOperations::Digest(edits, "agent");
  const auto queue = operations.Queue(project);
  std::lock_guard executing(*queue);
  std::string lane;
  {
    std::lock_guard lock(mutex);
    auto existing = db.prepare(
        "SELECT lane,digest FROM operation_writers WHERE operation=?");
    existing.bind(1, operation);
    if (existing.step()) {
      lane = existing.text(0);
      if (existing.text(1) != digest)
        throw Failure(
            "operation_conflict",
            "tool operation was reused with a different semantic payload");
    }
    if (lane.empty() && recovering)
      throw Failure("outcome_unknown",
                    "interrupted edit has no durable writer identity; inspect "
                    "before making a new edit");
  }
  if (lane.empty()) {
    {
      std::lock_guard lock(mutex);
      // Only abandoned identity mappings expire. Core still rejects their
      // unknown/expired lanes; this never causes automatic replay.
      auto expire = db.prepare("DELETE FROM operation_writers WHERE created<?");
      expire
          .bind(1, static_cast<int64_t>(std::time(nullptr) - 7 * 24 * 60 * 60))
          .run();
      auto count =
          db.prepare("SELECT COUNT(*) FROM operation_writers WHERE project=?");
      count.bind(1, project);
      count.step();
      if (count.u64(0) >= 64)
        throw Failure("resource_limit",
                      "64 outstanding operation identities; resolve them "
                      "before submitting more");
    }
    lane = json::Text(
        Outcome(RawProjectCall(project, "writer_open", "{}", stop)), "lane");
    std::lock_guard lock(mutex);
    auto record =
        db.prepare("INSERT INTO operation_writers VALUES(?,?,?,?,?,?,?)");
    record.bind(1, operation)
        .bind(2, project)
        .bind(3, conversation)
        .bind(4, call.id)
        .bind(5, lane)
        .bind(6, digest)
        .bind(7, static_cast<int64_t>(std::time(nullptr)))
        .run();
  }
  R::OperationSubmissionT identity;
  identity.identity = std::make_unique<R::OperationIdentityT>();
  identity.identity->lane = lane;
  identity.identity->sequence = 1;
  identity.semantic_digest = digest;
  const auto authorization =
      boundary ? boundary->authorization() : std::string{};
  host::OperationContext context;
  context.origin = "agent";
  context.authorize_file = [&](const std::string& path,
                               const host::SaveBase& base) {
    if (!boundary || !boundary->scoped()) return;
    // Project documents are read by the host only; the expanded graph diff is
    // checked below. Source and definition reads require the exact read set.
    if (path == "tsugumi.json" ||
        host::IsExternalDocument(path, "tsugumi.json"))
      return;
    const auto read = HelperReader(
        conversation, stop, json::Get(base.snapshot, "rev"), authorization);
    boundary->Check("project_file_read", {{"path", json::Quote(path)}}, read);
  };
  context.authorize = [&](json::Members& plan, const host::SaveBase& base) {
    {
      std::lock_guard lock(mutex);
      if (boundary) RequireHelperPermission(conversation, *boundary);
      const auto resource = authorization.empty() ? project : authorization;
      for (const auto grant :
           {Tsugumi::Service::Access::Read, Tsugumi::Service::Access::Write})
        if (!access_service.HasGrant(resource, conversation, grant))
          throw Failure("permission_denied",
                        "project grant was revoked before commit");
    }
    if (boundary && boundary->scoped()) {
      const auto read = HelperReader(
          conversation, stop, json::Get(base.snapshot, "rev"), authorization);
      boundary->CheckCurrentSave(plan, read);
    }
    if (args.contains("approved_permissions"))
      plan["approved_permissions"] = args.at("approved_permissions");
  };
  return OperateProject(project, identity, edits, context, stop).json;
}

auto Tools::AcknowledgeEdit(const std::string& project,
                            const std::string& operation, std::stop_token stop)
    -> void {
  std::string lane, digest;
  {
    std::lock_guard lock(mutex);
    auto row = db.prepare(
        "SELECT lane,digest FROM operation_writers WHERE operation=?");
    row.bind(1, operation);
    if (!row.step()) return;
    lane = row.text(0);
    digest = row.text(1);
  }
  const auto status = Outcome(RawProjectCall(
      project, "writer_status",
      json::Encode(json::Members{{"operation", Submission(lane, digest)}}),
      stop));
  if (json::Text(status, "state") != "completed") return;
  const auto receipt = json::Object(json::Get(status, "receipt"));
  (void)Outcome(RawProjectCall(
      project, "writer_acknowledge",
      json::Encode(json::Members{
          {"identity", json::Encode(json::Members{{"lane", json::Quote(lane)},
                                                  {"sequence", "\"1\""}})},
          {"incorporated_cursor", json::Get(receipt, "cursor")},
          {"retire", "true"}}),
      stop));
  std::lock_guard lock(mutex);
  auto remove = db.prepare("DELETE FROM operation_writers WHERE operation=?");
  remove.bind(1, operation).run();
}
}  // namespace tsugumi::agent
