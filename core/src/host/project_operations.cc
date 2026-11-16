#include "host/project_operations.hh"

#include "agent/provider.hh"
#include "agent/save_policy.hh"
#include "host/failure.hh"
#include "host/layout_saving.hh"
#include "host/layout_service.hh"
#include "util/crypto.hh"

namespace tsugumi::host {
namespace {
namespace H = Tsugumi::Host;
namespace json = agent::json;
auto Value(const OperationReply& reply) -> json::Members {
  const auto envelope = json::Object(reply.json);
  const auto value = json::Object(json::Get(envelope, "value", "{}"));
  if (json::Text(envelope, "reply") == "Failure")
    throw Failure(json::Text(value, "kind"), json::Text(value, "diagnostic"));
  return value;
}
auto Bytes(const std::string& value) -> std::string {
  if (value.starts_with('"')) return json::String(value);
  std::string decoded;
  if (!crypto::base64_decode(json::Text(json::Object(value), "base64"),
                             decoded))
    throw Failure("invalid_request", "invalid file bytes");
  return decoded;
}
auto CanonicalJson(const std::string& input) -> std::string {
  schema::Json valid;
  if (auto error = schema::decode(input, valid))
    throw Failure("validation_failed", error->message);
  const auto raw = schema::text(valid.value());
  if (raw.starts_with('{')) {
    auto fields = json::Object(raw);
    for (auto& [key, value] : fields) {
      (void)key;
      value = CanonicalJson(value);
    }
    return json::Encode(fields);
  }
  if (raw.starts_with('[')) {
    auto items = json::Array(raw);
    for (auto& item : items) item = CanonicalJson(item);
    return json::Encode(items);
  }
  return raw.starts_with('"') ? json::Quote(json::String(raw)) : raw;
}
auto Submission(const Tsugumi::RPC::OperationSubmissionT& operation)
    -> std::string {
  return json::Encode(json::Members{
      {"identity", json::Encode(json::Members{
                       {"lane", json::Quote(operation.identity->lane)},
                       {"sequence", json::Quote(std::to_string(
                                        operation.identity->sequence))}})},
      {"semantic_digest", json::Quote(operation.semantic_digest)}});
}
auto OperationId(const Tsugumi::RPC::OperationIdentityT& identity)
    -> std::string {
  auto hash = crypto::sha256_hex(identity.lane + ':' +
                                 std::to_string(identity.sequence));
  return hash.substr(0, 8) + '-' + hash.substr(8, 4) + "-4" +
         hash.substr(13, 3) + "-8" + hash.substr(17, 3) + '-' +
         hash.substr(20, 12);
}
}  // namespace

auto ProjectOperations::Digest(const H::ProjectEditsT& edits,
                               const std::string& origin) -> std::string {
  auto normalized = edits;
  for (auto& edit : normalized.edits)
    if (auto* field = edit->value.AsProjectFieldEdit()) {
      for (auto& step : field->selector)
        if (!step->match_json.empty())
          step->match_json = CanonicalJson(step->match_json);
      if (field->old_value && field->old_value->present)
        field->old_value->json = CanonicalJson(field->old_value->json);
      if (field->new_value && field->new_value->present)
        field->new_value->json = CanonicalJson(field->new_value->json);
    }
  flatbuffers::FlatBufferBuilder builder;
  builder.Finish(H::ProjectEdits::Pack(builder, &normalized));
  return crypto::sha256_hex(
      origin + '\n' +
      std::string(reinterpret_cast<const char*>(builder.GetBufferPointer()),
                  builder.GetSize()));
}

auto ProjectOperations::Queue(const std::string& project)
    -> std::shared_ptr<std::recursive_mutex> {
  std::lock_guard lock(mutex_);
  std::erase_if(queues_,
                [](const auto& item) { return item.second.expired(); });
  auto queue = queues_[project].lock();
  if (!queue) {
    queue = std::make_shared<std::recursive_mutex>();
    queues_[project] = queue;
  }
  return queue;
}

auto ProjectOperations::Execute(
    const std::string& project,
    const Tsugumi::RPC::OperationSubmissionT& operation,
    const H::ProjectEditsT& edits, const OperationContext& context,
    const OperationCall& call) -> OperationReply {
  if (!operation.identity ||
      operation.semantic_digest != Digest(edits, context.origin))
    throw Failure(
        "operation_conflict",
        "semantic digest differs from the supplied operation body or actor");
  const auto queue = Queue(project);
  std::lock_guard executing(*queue);
  const auto submission = Submission(operation);
  const auto id = OperationId(*operation.identity);
  // Resolve a lost reply before reading current documents or computing layout.
  (void)Value(call("writer_begin", {{"operation", submission}}));
  const auto status = Value(call("writer_status", {{"operation", submission}}));
  if (json::Text(status, "state") == "completed") {
    // Resubmit an empty concrete plan with the same semantic identity. Core
    // replays before validating the plan or its revision, returning original
    // wire.
    return call("project_save", {{"op_id", json::Quote(id)},
                                 {"base_rev", "0"},
                                 {"origin", json::Quote(context.origin)},
                                 {"changes", "[]"},
                                 {"operation", submission}});
  }
  auto refuse = [&](std::string kind, const std::string& message) {
    if (kind == "target_missing") kind = "not_found";
    if (kind == "value_conflict") kind = "revision_conflict";
    if (kind == "source_changed") kind = "hash_conflict";
    bool known = false;
    for (auto code : Tsugumi::Data::EnumValuesFailureKind())
      if (kind == Tsugumi::Data::EnumNameFailureKind(code)) known = true;
    if (!known) kind = "validation_failed";
    return call("operation_refuse",
                {{"operation", submission},
                 {"op_id", json::Quote(id)},
                 {"failure", json::Encode(json::Members{
                                 {"kind", json::Quote(kind)},
                                 {"diagnostic", json::Quote(message)},
                                 {"args", "[]"}})}});
  };
  try {
    for (int attempt = 0; attempt < 8; ++attempt) {
      const auto inspection =
          InspectForSave([&](const auto& method, const auto& query) {
            return Value(call(method, query));
          });
      auto read = [&](const std::string& path) -> std::optional<std::string> {
        std::string hash;
        for (const auto& raw :
             json::Array(json::Get(inspection.snapshot, "sources", "[]"))) {
          const auto source = json::Object(raw);
          if (json::Text(source, "path") == path)
            hash = json::Text(source, "hash");
        }
        if (hash.empty()) return std::nullopt;
        std::string bytes;
        for (;;) {
          const auto page =
              Value(call("project_file_read",
                         {{"path", json::Quote(path)},
                          {"at_rev", json::Get(inspection.snapshot, "rev")},
                          {"if_hash", json::Quote(hash)},
                          {"offset", std::to_string(bytes.size())},
                          {"limit_bytes", "1048576"}}));
          const auto chunk = Bytes(json::Get(page, "bytes"));
          bytes += chunk;
          if (json::Bool(json::Get(page, "eof"))) break;
          if (chunk.empty())
            throw Failure("unavailable", "file read made no progress: " + path);
        }
        return bytes;
      };
      try {
        auto plan = PrepareProjectEdits(
            edits, inspection.base, read, [&](const std::string& path) {
              if (context.authorize_file)
                context.authorize_file(path, inspection.base);
            });
        plan["op_id"] = json::Quote(id);
        plan["origin"] = json::Quote(context.origin);
        auto layout_read =
            [&](const std::string& path) -> std::optional<LayoutFile> {
          auto bytes = inspection.base.files.contains(path)
                           ? std::optional(inspection.base.files.at(path))
                           : read(path);
          return bytes ? std::optional<LayoutFile>{{*bytes,
                                                    crypto::sha256_hex(*bytes)}}
                       : std::nullopt;
        };
        if (context.origin == "agent")
          plan = agent::PrepareAgentSave(plan, [&](const auto& method,
                                                   const auto& args) {
                   return Value(call(method, args));
                 }).arguments;
        plan = json::Object(PrepareLayoutSave(json::Encode(plan), layout_read));
        if (context.authorize) context.authorize(plan, inspection.base);
        plan["check_permissions"] = "true";
        plan["operation"] = submission;
        auto result = call("project_save", plan);
        const auto envelope = json::Object(result.json);
        if (json::Text(envelope, "reply") != "Failure") return result;
        const auto failure = json::Object(json::Get(envelope, "value", "{}"));
        const auto kind = json::Text(failure, "kind");
        if (kind == "persistence_failed" || kind == "internal_error" ||
            kind == "timeout")
          throw Failure("outcome_unknown",
                        "operation has no proven terminal outcome; inspect its "
                        "original identity after reconnecting");
        if (kind != "revision_conflict") return result;
      } catch (const agent::SaveRevisionConflict&) {
        continue;
      } catch (const Failure& failure) {
        if (failure.code != "revision_conflict") throw;
      }
    }
    throw Failure("busy",
                  "project kept changing during operation preparation; retry "
                  "the same identity");
  } catch (const Failure& failure) {
    if (failure.code == "approval_required" || failure.code == "unavailable" ||
        failure.code == "outcome_unknown")
      throw;
    return refuse(failure.code, failure.what());
  } catch (const agent::Cancelled&) {
    throw;
  } catch (const LayoutFailure& failure) {
    return refuse("invalid_document", failure.what());
  } catch (const std::exception& failure) {
    return refuse("validation_failed", failure.what());
  }
}
}  // namespace tsugumi::host
