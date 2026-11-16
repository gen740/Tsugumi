#include "host/project_operations.hh"

#include <algorithm>
#include <set>

#include "agent/provider.hh"
#include "agent/save_policy.hh"
#include "host/document_edits.hh"
#include "host/failure.hh"
#include "host/layout_service.hh"
#include "util/crypto.hh"
#include "util/fs.hh"

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
auto Change(const std::string& kind, json::Members value) -> std::string {
  return json::Encode(json::Members{{"value_type", json::Quote(kind)},
                                    {"value", json::Encode(value)}});
}
auto Prefix(const std::string& path) -> std::string {
  if (path.empty()) return {};
  if (!fs::valid_relative_path(path) ||
      !IsExternalDocument(path + "/tsugumi.json", "tsugumi.json"))
    throw Failure("invalid_path", "project_path must name an external project");
  return path + '/';
}
auto Intent(const H::ProjectOperateRequestT& operation,
            const std::string& origin) -> std::string {
  auto semantic = operation;
  semantic.op_id.clear();
  flatbuffers::FlatBufferBuilder builder;
  builder.Finish(H::ProjectOperateRequest::Pack(builder, &semantic));
  crypto::Sha256 hash;
  hash.update(origin + '\n');
  hash.update(builder.GetBufferPointer(), builder.GetSize());
  return hash.finish_hex();
}
// Expand transformations into guarded file edits against this inspection only.
auto Prepare(const H::ProjectOperationBodyUnion& body, const SaveBase& base,
             const EditFileRead& read,
             const std::function<void(const std::string&)>& authorize)
    -> json::Members {
  if (const auto* edits = body.AsProjectEdits())
    return PrepareProjectEdits(*edits, base, read, authorize);
  H::ProjectEditsT edits;
  std::vector<std::string> removed_definitions;
  auto get = [&](const std::string& path) -> std::optional<std::string> {
    if (authorize) authorize(path);
    if (base.files.contains(path)) return base.files.at(path);
    return read(path);
  };
  auto put = [&](const std::string& path, std::string bytes) {
    const auto before = get(path);
    if (before && *before == bytes) return;
    if (before && base.files.contains(path))
      bytes = PreserveProjectBytes(*before, bytes);
    H::ProjectFilePutT value;
    value.file_path = path;
    value.expect_missing = !before;
    if (before) value.expected_hash = crypto::sha256_hex(*before);
    value.bytes.assign(bytes.begin(), bytes.end());
    auto entry = std::make_unique<H::ProjectEditT>();
    entry->value.Set(std::move(value));
    edits.edits.push_back(std::move(entry));
  };
  auto remove = [&](const std::string& path) {
    const auto before = get(path);
    if (!before) throw Failure("not_found", path);
    H::ProjectFileRemoveT value;
    value.file_path = path;
    value.expected_hash = crypto::sha256_hex(*before);
    auto entry = std::make_unique<H::ProjectEditT>();
    entry->value.Set(std::move(value));
    edits.edits.push_back(std::move(entry));
  };
  auto required = [&](const std::string& path) {
    const auto bytes = get(path);
    if (!bytes) throw Failure("not_found", path);
    return *bytes;
  };
  std::string prefix;
  if (const auto* connect = body.AsProjectConnect())
    prefix = Prefix(connect->project_path);
  else if (const auto* subgraph = body.AsProjectMakeSubgraph())
    prefix = Prefix(subgraph->project_path);
  else if (const auto* layout = body.AsProjectLayout())
    prefix = Prefix(layout->project_path);
  else
    throw Failure("invalid_request", "missing operation body");
  const auto project = required(prefix + "tsugumi.json");
  if (const auto* connect = body.AsProjectConnect()) {
    json::Members definitions;
    for (const auto& raw : json::Array(json::Get(
             json::Object(json::Get(json::Object(project), "canvas", "{}")),
             "cards", "[]"))) {
      const auto id = json::Text(json::Object(raw), "id");
      json::Members parts;
      for (const auto& [part, name] : {std::pair{"card", "_card.json"},
                                       {"meta", "_meta.json"},
                                       {"link", "_link.json"}}) {
        const auto path = prefix + "definitions/" + id + '/' + name;
        // Only definitions that influence this connection need a helper read
        // grant.
        if (base.files.contains(path)) parts[part] = base.files.at(path);
      }
      definitions[id] = json::Encode(parts);
    }
    const auto connected = ConnectDocuments(
        json::Encode(json::Members{{"project", project},
                                   {"definitions", json::Encode(definitions)}}),
        connect->from_card, connect->from_port, connect->to_card,
        connect->to_port, connect->fork_id);
    if (connected.needs_fork)
      throw Failure("invalid_request",
                    "Reserve a fork Card id and supply fork_id");
    const auto state = json::Object(connected.state_json);
    put(prefix + "tsugumi.json", state.at("project"));
    for (const auto& [id, raw] : json::Object(state.at("definitions"))) {
      const auto parts = json::Object(raw);
      for (const auto& [part, name] : {std::pair{"card", "_card.json"},
                                       {"meta", "_meta.json"},
                                       {"link", "_link.json"}})
        if (parts.contains(part) && (!definitions.contains(id) ||
                                     json::Get(json::Object(definitions.at(id)),
                                               part) != parts.at(part)))
          put(prefix + "definitions/" + id + '/' + name, parts.at(part));
    }
  } else if (const auto* subgraph = body.AsProjectMakeSubgraph()) {
    const auto nested = MakeSubgraph(project, subgraph->cards, subgraph->id);
    const auto destination = prefix + "externals/" + subgraph->id + '/';
    if (get(destination + "tsugumi.json") ||
        get(prefix + "definitions/" + subgraph->id + "/_card.json") ||
        get(prefix + "definitions/" + subgraph->id + "/_link.json"))
      throw Failure("value_conflict", "subgraph destination already exists");
    put(prefix + "tsugumi.json", nested.project_json);
    put(destination + "tsugumi.json",
        PreserveProjectBytes(project, nested.external_json));
    put(prefix + "definitions/" + subgraph->id + "/_link.json",
        nested.link_json);
    std::set<std::string> paths;
    for (const auto& [path, ignored] : base.files) {
      (void)ignored;
      paths.insert(path);
    }
    for (const auto& raw :
         json::Array(json::Get(base.snapshot, "sources", "[]")))
      paths.insert(json::Text(json::Object(raw), "path"));
    auto positions = json::Object(json::Get(
        json::Object(required(prefix + "layout.json")), "cards", "{}"));
    json::Members inner_positions;
    std::set<std::string> copied;
    for (const auto& id : subgraph->cards) {
      const auto definition = prefix + "definitions/" + id + '/';
      const auto link = get(definition + "_link.json");
      const auto external =
          link ? prefix + "externals/" +
                     json::Text(json::Object(*link), "external") + '/'
               : std::string{};
      bool shared_external = false;
      if (!external.empty()) {
        for (const auto& [path, bytes] : base.files) {
          if (!path.starts_with(prefix + "definitions/") ||
              !path.ends_with("/_link.json"))
            continue;
          const auto local = path.substr(prefix.size() + 12);
          const auto other = local.substr(0, local.find('/'));
          if (std::find(subgraph->cards.begin(), subgraph->cards.end(),
                        other) == subgraph->cards.end() &&
              prefix + "externals/" +
                      json::Text(json::Object(bytes), "external") + '/' ==
                  external)
            shared_external = true;
        }
      }
      for (const auto& path : paths) {
        if (!path.starts_with(definition) &&
            (external.empty() || !path.starts_with(external)))
          continue;
        if (!copied.insert(path).second) continue;
        put(destination + path.substr(prefix.size()), required(path));
        // Root definition removal includes its sources. External files and
        // nested definitions use the ordinary concrete source removals.
        if (path.starts_with(definition) ? !prefix.empty() : !shared_external)
          remove(path);
      }
      if (prefix.empty())
        removed_definitions.push_back(
            Change("RemoveDefinition", {{"id", json::Quote(id)}}));
      if (positions.contains(id)) inner_positions[id] = positions.at(id);
    }
    put(destination + "layout.json",
        json::Encode(json::Members{{"cards", json::Encode(inner_positions)}}));
  } else if (const auto* layout = body.AsProjectLayout()) {
    if (layout->action != H::ProjectLayoutAction::Arrange &&
        layout->action != H::ProjectLayoutAction::Untangle)
      throw Failure("invalid_request", "unknown layout action");
    const auto path = prefix + "layout.json";
    put(path, ArrangeProjectLayout(
                  project, required(path),
                  [&](const auto& relative) -> std::optional<LayoutFile> {
                    const auto bytes = get(prefix + relative);
                    return bytes ? std::optional<LayoutFile>{{*bytes, {}}}
                                 : std::nullopt;
                  },
                  layout->action == H::ProjectLayoutAction::Untangle));
  }
  auto plan = edits.edits.empty()
                  ? json::Members{{"base_rev", json::Get(base.snapshot, "rev")},
                                  {"changes", "[]"}}
                  : PrepareProjectEdits(edits, base, read, authorize);
  auto changes = json::Array(plan.at("changes"));
  changes.insert(changes.end(), removed_definitions.begin(),
                 removed_definitions.end());
  // An unchanged layout/connect still uses the existing save idempotence path.
  if (changes.empty())
    changes.push_back(Change("SetProject", {{"json", json::Quote(project)}}));
  plan["changes"] = json::Encode(changes);
  return plan;
}
// Preserve policy/placement changes relative to the bytes explicitly authored
// by this operation, so even an exact whitespace replacement is retained.
auto Authored(const json::Members& plan, SaveBase base) -> SaveBase {
  for (const auto& raw : json::Array(json::Get(plan, "changes", "[]"))) {
    const auto change = json::Object(raw);
    const auto type = json::Text(change, "value_type");
    const auto value = json::Object(json::Get(change, "value", "{}"));
    if (type == "SetProject" || type == "SetLayout")
      base.files[type == "SetProject" ? "tsugumi.json" : "layout.json"] =
          json::Text(value, "json");
    else if (type == "PutDefinition") {
      const auto dir = "definitions/" + json::Text(value, "id") + '/';
      for (const auto& [key, file] : {std::pair{"card_json", "_card.json"},
                                      {"meta_json", "_meta.json"},
                                      {"link_json", "_link.json"}})
        if (value.contains(key)) {
          const auto text = json::Text(value, key);
          if (text.empty())
            base.files.erase(dir + file);
          else
            base.files[dir + file] = text;
        }
    } else if (type == "PutSource" &&
               base.files.contains(json::Text(value, "path")))
      base.files[json::Text(value, "path")] = Bytes(json::Get(value, "bytes"));
  }
  return base;
}
// Policy and placement may return re-encoded JSON. Only their changed spans
// belong to this operation; every other document byte remains the original.
auto Preserve(json::Members plan, const SaveBase& base) -> json::Members {
  auto changes = json::Array(json::Get(plan, "changes", "[]"));
  for (auto& raw : changes) {
    auto change = json::Object(raw);
    auto value = json::Object(json::Get(change, "value", "{}"));
    const auto type = json::Text(change, "value_type");
    auto preserve = [&](const std::string& path, const std::string& key) {
      if (base.files.contains(path) && value.contains(key) &&
          json::Get(value, key) != "null")
        value[key] = json::Quote(
            PreserveProjectBytes(base.files.at(path), json::Text(value, key)));
    };
    if (type == "SetProject")
      preserve("tsugumi.json", "json");
    else if (type == "SetLayout")
      preserve("layout.json", "json");
    else if (type == "PutDefinition") {
      const auto dir = "definitions/" + json::Text(value, "id") + '/';
      for (const auto& [key, name] : {std::pair{"card_json", "_card.json"},
                                      {"meta_json", "_meta.json"},
                                      {"link_json", "_link.json"}})
        preserve(dir + name, key);
    } else if (type == "PutSource" &&
               base.files.contains(json::Text(value, "path"))) {
      const auto path = json::Text(value, "path");
      value["bytes"] = json::Quote(PreserveProjectBytes(
          base.files.at(path), Bytes(json::Get(value, "bytes"))));
    }
    change["value"] = json::Encode(value);
    raw = json::Encode(change);
  }
  plan["changes"] = json::Encode(changes);
  return plan;
}
}  // namespace

auto ProjectOperations::Execute(const std::string& project,
                                const H::ProjectOperateRequestT& operation,
                                const OperationContext& context,
                                const OperationCall& call) -> OperationReply {
  if (project != operation.project_id ||
      !crypto::is_operation_id(operation.op_id))
    throw Failure("internal_error",
                  "Internal error: the host operation has a mismatched project "
                  "or invalid operation id; retrying will not help.");
  const auto intent = Intent(operation, context.origin);
  // Reuse the core's existing save receipt, before interpreting old values that
  // a successful first application has already changed.
  const auto status =
      Value(call("operation_get", {{"op_id", json::Quote(operation.op_id)},
                                   {"save_intent", json::Quote(intent)}}));
  if (json::Text(status, "state") == "completed")
    return call("project_save", {{"op_id", json::Quote(operation.op_id)},
                                 {"save_intent", json::Quote(intent)},
                                 {"base_rev", "0"},
                                 {"origin", json::Quote(context.origin)},
                                 {"changes", "[]"}});
  for (int attempt = 0; attempt < 8; ++attempt) {
    try {
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
      auto plan =
          Prepare(operation.body, inspection.base, read, [&](const auto& path) {
            if (context.authorize_file)
              context.authorize_file(path, inspection.base);
          });
      if (json::Array(plan.at("changes")).empty())
        plan["changes"] = json::Encode(std::vector<std::string>{
            Change("SetProject", {{"json", json::Quote(inspection.base.files.at(
                                               "tsugumi.json"))}})});
      const auto authored = Authored(plan, inspection.base);
      plan["op_id"] = json::Quote(operation.op_id);
      plan["origin"] = json::Quote(context.origin);
      if (context.origin == "agent" && operation.body.AsProjectEdits())
        plan = agent::PrepareAgentSave(plan, [&](const auto& method,
                                                 const auto& args) {
                 return Value(call(method, args));
               }).arguments;
      auto layout_read =
          [&](const std::string& path) -> std::optional<LayoutFile> {
        const auto bytes = inspection.base.files.contains(path)
                               ? std::optional(inspection.base.files.at(path))
                               : read(path);
        return bytes ? std::optional<LayoutFile>{{*bytes,
                                                  crypto::sha256_hex(*bytes)}}
                     : std::nullopt;
      };
      std::vector<std::string> documents{"tsugumi.json"};
      for (const auto& [path, ignored] : inspection.base.files) {
        (void)ignored;
        if (IsExternalDocument(path, "tsugumi.json")) documents.push_back(path);
      }
      plan = Preserve(
          json::Object(PrepareLayoutSave(
              json::Encode(plan), layout_read, documents,
              context.layout_scope ? context.layout_scope(inspection.base)
                                   : LayoutWrite{})),
          authored);
      if (context.authorize) context.authorize(plan, inspection.base);
      plan["check_permissions"] = context.origin == "agent" ? "true" : "false";
      plan["save_intent"] = json::Quote(intent);
      const auto result = call("project_save", plan);
      const auto envelope = json::Object(result.json);
      if (json::Text(envelope, "reply") != "Failure" ||
          json::Text(json::Object(json::Get(envelope, "value", "{}")),
                     "kind") != "revision_conflict")
        return result;
    } catch (const agent::SaveRevisionConflict&) {
      continue;
    } catch (const Failure& failure) {
      if (failure.code != "revision_conflict") throw;
    } catch (const agent::Cancelled&) {
      throw;
    } catch (const LayoutFailure& failure) {
      throw Failure("invalid_document", failure.what());
    } catch (const std::exception& failure) {
      throw Failure("validation_failed", failure.what());
    }
  }
  throw Failure("busy", "Project kept changing; retry the same operation id");
}
}  // namespace tsugumi::host
