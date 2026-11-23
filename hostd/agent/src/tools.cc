#include "agent/tools.hh"

#include "host/layout_file.hh"

#include <algorithm>
#include <condition_variable>
#include <future>
#include <map>
#include <set>

#include "agent/approval.hh"
#include "agent/context.hh"
#include "agent/helpers.hh"
#include "agent/providers.hh"
#include "agent/tool_images.hh"
#include "agent/version_find.hh"
#include "agent/turns.hh"
#include "agent/wire.hh"
#include "host/access.hh"
#include "host/agent_policy.hh"
#include "host/attached_folder.hh"
#include "host/demo_references.hh"
#include "host/document_edits.hh"
#include "host/failure.hh"
#include "host/file_miss.hh"
#include "host/format.hh"
#include "host/identifiers.hh"
#include "host/input_types.hh"
#include "host/layout_saving.hh"
#include "host/layout_service.hh"
#include "host/save.hh"
#include "host/settings.hh"
#include "host/templates.hh"
#include "host/version_text.hh"
#include "tsugumi/client/client.hh"
#include "tsugumi/protocol/tsugumi_generated.h"
#include "tsugumi/util/crypto.hh"

namespace tsugumi::agent {
namespace A = Tsugumi::Agent;
namespace H = Tsugumi::Host;
namespace R = Tsugumi::RPC;
using host::Failure;
namespace {
auto Text(const flatbuffers::String* value) -> std::string {
  return value ? value->str() : "";
}
auto LayoutToolFailure(const std::string& kind, const std::string& message)
    -> std::string {
  return json::Encode(json::Members{
      {"reply", json::Quote("Failure")},
      {"value", json::Encode(json::Members{{"kind", json::Quote(kind)},
                                           {"diagnostic", json::Quote(message)},
                                           {"args", "[]"}})}});
}

auto ToolOutput(std::string_view raw, bool error,
                std::vector<std::string> resources) -> ToolResult {
  auto split = SplitToolImages(raw);
  split.text = host::AgentReferences(split.text);
  return {std::move(split.text), error, std::move(split.images),
          std::move(resources)};
}

}  // namespace

Tools::Tools(host::StateGate& gate, sql::Db& database, ConversationStore& store,
             Providers& selections, host::Settings& settings,
             host::DemoReferences& demos, Turns& turns, std::stop_source& stop,
             host::Access& access, Dependencies dependencies)
    : mutex(gate.mutex),
      db(database),
      conversations(&store),
      providers(&selections),
      host_settings(&settings),
      demo_references(demos),
      registry(store.registry),
      turns(&turns),
      shutdown(stop),
      access_service(access),
      dependencies(std::move(dependencies)) {}

auto Tools::Specs(bool guide) -> std::vector<ToolSpec> {
  return catalog.ModelSpecs(guide ? ModelRole::Guide : ModelRole::Main);
}

// Read the exact revision being saved; never clean up a newer client's layout.
auto Tools::PrepareLayoutSave(const std::string& project,
                              const std::string& args, std::stop_token stop)
    -> std::string {
  std::optional<host::SaveBase> base;
  const auto fields = json::Object(args);
  const auto expected =
      json::Get(fields, fields.contains("base_rev") ? "base_rev" : "at_rev");
  struct StaleRevision {};
  auto read = [&](const std::string& path) -> std::optional<host::LayoutFile> {
    if (!base) {
      const auto response =
          json::Object(RawProjectCall(project, "project_inspect", "{}", stop));
      const auto snapshot = json::Object(json::Get(response, "value", "{}"));
      if (json::Text(response, "reply") == "Failure")
        throw Failure(json::Text(snapshot, "kind"),
                      json::Text(snapshot, "diagnostic"));
      const auto revision = json::Get(snapshot, "rev");
      if (json::String(expected, expected) != json::String(revision, revision))
        throw StaleRevision{};
      base = host::CaptureSaveBase(snapshot);
    }
    const auto file = base->files.find(path);
    if (file == base->files.end()) return std::nullopt;
    return host::LayoutFile{file->second, crypto::sha256_hex(file->second)};
  };
  try {
    return host::PrepareLayoutSave(args, read);
  } catch (const StaleRevision&) {
    return args;
  }  // Let the core issue its normal CAS receipt.
}

auto Tools::OperateProject(const H::ProjectOperateRequestT& operation,
                           const host::OperationContext& context,
                           std::stop_token stop) -> host::OperationReply {
  std::lock_guard saving(save_mutex);
  const auto& project = operation.project_id;
  auto resolved_context = context;
  resolved_context.host_templates = dependencies.host_templates;
  if (context.origin == "agent" && operation.body.AsProjectTemplateCopy()) {
    resolved_context.reserve_template_id = [&](const auto& source,
                                               const auto& allocate) {
      // Approval names the destination Card. Remember the core's
      // reservation across retries of the same operation.
      const auto scope =
          "template-copy:" +
          crypto::sha256_hex(project + '\n' + operation.op_id + '\n' + source);
      {
        std::lock_guard lock(mutex);
        auto reserved = db.prepare("SELECT id FROM identifiers WHERE scope=?");
        reserved.bind(1, scope);
        if (reserved.step()) return reserved.text(0);
      }
      const auto id = allocate();
      std::lock_guard lock(mutex);
      db.prepare("INSERT INTO identifiers VALUES(?,?)")
          .bind(1, scope)
          .bind(2, id)
          .run();
      return id;
    };
  }
  if (context.origin == "ui")
    resolved_context.read_template_source = [&](const auto& source) {
      return host::ReadTemplateSource(source, stop);
    };
  resolved_context.demo = [&](const auto& name) {
    const auto folder = demo_references.Project(name);
    if (context.origin == "agent") Exclusions(project, stop).Check(folder);
    return folder;
  };
  return operations.Execute(operation, resolved_context,
                            [&](const auto& method, const auto& arguments) {
                              host::OperationReply reply;
                              reply.json = RawProjectCall(
                                  project, method, json::Encode(arguments),
                                  stop, &reply.wire);
                              return reply;
                            });
}

auto Tools::ListTemplates(const std::string& project, bool agent,
                          std::stop_token stop) -> H::TemplateListT {
  const auto base = host::CaptureSaveBase(InspectProject(project, stop));
  const auto excluded =
      agent ? std::optional(ExclusionsFor(project, base)) : std::nullopt;
  auto visible = [&](const auto& path) {
    return !excluded || !excluded->Contains(path);
  };
  H::TemplateListT result;
  auto entry = [&](H::TemplateStore store, const auto& name) {
    auto item = std::make_unique<H::TemplateEntryT>();
    item->store = store;
    item->name = name;
    result.entries.push_back(std::move(item));
  };
  std::set<std::string> names;
  std::set<std::string> hidden;
  for (const auto& path : host::ProjectPaths(base)) {
    if (!path.starts_with("templates/")) continue;
    const auto relative = std::string_view(path).substr(10);
    const auto slash = relative.find('/');
    if (slash == std::string_view::npos) continue;
    const std::string name(relative.substr(0, slash));
    if (!host::TemplateName(name)) continue;
    if (!visible(path)) hidden.insert(name);
    const auto card = relative.substr(slash + 1);
    if (card.ends_with("/_card.json") && card.find('/') == card.rfind('/'))
      names.insert(name);
  }
  for (const auto& name : names)
    if (!hidden.contains(name) && visible("templates/" + name))
      entry(H::TemplateStore::Project, name);
  host::TemplatePathCheck check;
  if (excluded) check = [&](const auto& path) { excluded->Check(path); };
  for (const auto& name :
       host::HostTemplateNames(dependencies.host_templates, check))
    entry(H::TemplateStore::Host, name);
  return result;
}

auto Tools::AddHostTemplate(const H::TemplateAddT& request,
                            std::stop_token stop,
                            const host::TemplatePathCheck& check) -> void {
  const auto contents = host::ReadTemplateSource(request, stop, check);
  std::lock_guard saving(save_mutex);
  host::AddHostTemplate(dependencies.host_templates, request.name, contents);
}

auto Tools::SaveProject(const std::string& project, json::Members original,
                        std::stop_token stop,
                        const std::function<void(const host::SaveBase&)>& check)
    -> std::string {
  std::lock_guard save_lock(save_mutex);
  try {
    // A whole-document save keeps its original revision and input. Never merge
    // or issue another operation id when another writer has moved the document.
    const auto latest = host::CaptureSaveBase(InspectProject(project, stop));
    if (json::String(json::Get(original, "base_rev"),
                     json::Get(original, "base_rev")) !=
        json::Text(latest.snapshot, "rev"))
      return LayoutToolFailure("revision_conflict",
                               "The project changed; read the project again "
                               "and re-apply your input.");
    const auto policy = ExclusionsFor(project, latest);
    const host::SourcePatches patches;
    policy.CheckSave(original, latest, patches);
    if (check) check(latest);
    original.erase("save_intent");
    const host::SavePreparation preparation{
        .stamp_agent = true,
        .reserve_comment =
            [&](const auto& card, const auto& occupied) {
              std::lock_guard lock(mutex);
              return host::ReserveOrdinal(
                  db, "annotation:" + project + ":comment:" + card, "",
                  occupied);
            },
        .finish =
            [&](auto& prepared) {
              policy.CheckSave(prepared, latest, patches);
              host::RestrictAgentRequest("project_save", prepared);
            }};
    auto submitted = host::SubmitProjectSave(
        std::move(original), latest, patches,
        [&](const auto& method, const auto& arguments) {
          return host::OperationReply{
              RawProjectCall(project, method, json::Encode(arguments), stop),
              {}};
        },
        preparation);
    auto result = json::Object(submitted.reply.json);
    if (json::Text(result, "reply") == "SaveResult") {
      auto value = json::Object(json::Get(result, "value"));
      value["comments"] = submitted.comments;
      result["value"] = json::Encode(value);
    } else if (json::Text(json::Object(json::Get(result, "value", "{}")),
                          "kind") == "revision_conflict") {
      return LayoutToolFailure("revision_conflict",
                               "The project changed; read the project again "
                               "and re-apply your input.");
    }
    return json::Encode(result);
  } catch (const host::LayoutFailure& e) {
    return LayoutToolFailure("invalid_document", e.what());
  } catch (const Cancelled&) {
    throw;
  } catch (const Failure& e) {
    // Policy validation failed before sending a save to the core. Keep that
    // refusal as a terminal receipt so replay cannot become outcome_unknown.
    if (e.code == "validation_failed")
      return LayoutToolFailure(e.code, e.what());
    throw;
  } catch (const std::exception& e) {
    return LayoutToolFailure("validation_failed", e.what());
  }
}

auto Tools::InputTypesQuery(const std::string& project, json::Members args,
                            std::stop_token stop) -> H::InputTypesT {
  std::string bytes;
  const auto envelope =
      RawProjectCall(project, "input_types", json::Encode(args), stop, &bytes);
  const auto* reply = R::GetMessage(bytes.data())->payload_as_Reply();
  if (!reply->value_as_InputTypesResult()) {
    const auto value =
        json::Object(json::Get(json::Object(envelope), "value", "{}"));
    throw Failure(json::Text(value, "kind"), json::Text(value, "diagnostic"));
  }
  return host::InputTypes(*reply->value_as_InputTypesResult());
}

auto Tools::InspectProject(const std::string& project, std::stop_token stop)
    -> json::Members {
  const auto reply =
      json::Object(RawProjectCall(project, "project_inspect", "{}", stop));
  auto value = json::Object(json::Get(reply, "value", "{}"));
  if (json::Text(reply, "reply") == "Failure")
    throw Failure(json::Text(value, "kind"), json::Text(value, "diagnostic"));
  return value;
}

auto Tools::ExclusionsFor(const std::string& project,
                          const host::SaveBase& base) -> host::AgentExclude {
  const auto document = base.files.find("tsugumi.json");
  const auto json = document == base.files.end() ? "{}" : document->second;
  if (project.empty())
    return host::AgentExclude(std::filesystem::current_path(), json);
  std::lock_guard lock(mutex);
  return host::AgentExclude(registry.Root(project), json, base.files);
}

auto Tools::Exclusions(const std::string& project, std::stop_token stop)
    -> host::AgentExclude {
  return ExclusionsFor(
      project, project.empty()
                   ? host::SaveBase{}
                   : host::CaptureSaveBase(InspectProject(project, stop)));
}

namespace {
auto UsesRecordedDefinitions(const std::string& name) -> bool {
  return (name.starts_with("run_") && name != "run_plan" &&
          name != "run_history_delete") ||
         name.starts_with("artifact_") || name == "log_range";
}

// The evaluator can read every source staged for the Card and its exports
// producers. Walk the confirmed wiring before returning an expression's value.
auto EvaluationCards(const json::Members& snapshot, const std::string& card)
    -> std::vector<std::string> {
  std::map<std::string, std::vector<std::string>> producers;
  const auto key = [](const std::string& raw) {
    return json::Encode(
        json::Array(json::Get(json::Object(raw), "segments", "[]")));
  };
  for (const auto& raw : json::Array(json::Get(snapshot, "resolution", "[]"))) {
    const auto resolved = json::Object(raw);
    auto& targets = producers[key(resolved.at("card"))];
    for (const auto& ref :
         json::Array(json::Get(resolved, "nix_references", "[]")))
      targets.push_back(key(json::Object(ref).at("target")));
  }
  std::vector cards{key(card)};
  std::set seen{cards.front()};
  for (size_t i = 0; i < cards.size(); ++i)
    if (const auto it = producers.find(cards[i]); it != producers.end())
      for (const auto& target : it->second)
        if (seen.insert(target).second) cards.push_back(target);
  return cards;
}

auto CheckEvaluationSources(const host::AgentExclude& policy,
                            const host::SaveBase& base,
                            const json::Members& snapshot,
                            const json::Members& args) -> void {
  const auto cards = EvaluationCards(snapshot, json::Get(args, "card", "{}"));
  std::set<std::string> definitions;
  for (const auto& card : cards) {
    policy.CheckRequest("", {{"segments", card}});
    std::string prefix;
    const auto segments = json::Array(card);
    for (size_t i = 0; i < segments.size(); ++i) {
      const auto definition =
          prefix + "definitions/" + json::String(segments[i]) + '/';
      if (i + 1 == segments.size()) {
        definitions.insert(definition);
        break;
      }
      const auto it = base.files.find(definition + "_card.json");
      if (it == base.files.end()) break;
      const auto external = json::Text(json::Object(it->second), "external");
      if (external.empty()) break;
      prefix += "externals/" + external + '/';
    }
  }
  for (const auto& raw : json::Array(json::Get(snapshot, "sources", "[]"))) {
    const auto path = json::Text(json::Object(raw), "path");
    if (std::ranges::any_of(definitions, [&](const std::string& def) {
          return path.starts_with(def) && path != def + "_card.json" &&
                 path != def + "README.md" && path != def + "_comments.json";
        }))
      policy.Check(path);
  }
}
}  // namespace

auto Tools::ProjectCall(const std::string& project, const std::string& name,
                        const std::string& arguments, std::stop_token stop)
    -> std::string {
  const auto inspected = InspectProject(project, stop);
  const auto base = host::CaptureSaveBase(inspected);
  const auto policy = ExclusionsFor(project, base);
  auto args = json::Object(arguments);
  if (name == "log_range") args.erase("live");
  const bool recorded = UsesRecordedDefinitions(name);
  policy.CheckRequest(name, args, recorded);
  if (name == "project_inspect") {
    policy.Check("tsugumi.json");
    return policy.Filter(
        json::Encode(json::Members{{"reply", json::Quote("ProjectSnapshot")},
                                   {"value", json::Encode(inspected)}}));
  }
  if (name == "run_plan" || name == "evaluate_nix" ||
      name.starts_with("project_file_")) {
    const auto revision = json::Get(inspected, "rev");
    const auto number = [](const std::string& value) {
      return json::UInt(json::String(value, value));
    };
    if (args.contains("at_rev") &&
        number(args.at("at_rev")) != number(revision))
      throw Failure("revision_conflict", "The project changed; read it again");
    if (name != "project_file_format") args["at_rev"] = revision;
  }
  if (name == "evaluate_nix")
    CheckEvaluationSources(policy, base, inspected, args);
  auto check_recorded = [&](const std::string& run, const std::string& card) {
    policy.Check(".tsugumi/runs/" + run);
    json::Members query{{"run_id", json::Quote(run)}};
    if (!card.empty()) query["card"] = card;
    const auto envelope = json::Object(RawProjectCall(
        project, "run_definition_paths", json::Encode(query), stop));
    const auto value = json::Object(json::Get(envelope, "value", "{}"));
    if (json::Text(envelope, "reply") == "Failure")
      throw Failure(json::Text(value, "kind"), json::Text(value, "diagnostic"));
    for (const auto& path : json::Array(value.at("paths")))
      policy.Check(json::String(path));
  };
  if (recorded && args.contains("run_id"))
    check_recorded(json::Text(args, "run_id"), json::Get(args, "card"));
  if (name == "run_environment_paths" && args.contains("excluding_run_id"))
    check_recorded(json::Text(args, "excluding_run_id"), "");
  if (name == "artifact_read") {
    const auto id = json::Text(args, "artifact_id");
    const auto first = id.find('/'), last = id.rfind('/');
    const auto invocation = last == id.npos ? id.npos : id.rfind('/', last - 1);
    if (first == id.npos || invocation == id.npos || invocation <= first)
      throw Failure("not_found", "Artifact not found");
    std::vector<std::string> segments;
    for (size_t start = first + 1; start < invocation;) {
      const auto end = id.find('/', start);
      segments.push_back(json::Quote(id.substr(start, end - start)));
      start = end + 1;
    }
    check_recorded(
        id.substr(0, first),
        json::Encode(json::Members{{"segments", json::Encode(segments)}}));
  }
  if (name == "run") {
    json::Members plan;
    for (const auto* key : {"card", "propagate"})
      if (args.contains(key)) plan[key] = args.at(key);
    plan["at_rev"] = json::Get(inspected, "rev");
    const auto base = json::Get(args, "base_rev");
    if (json::String(base, base) !=
        json::String(plan.at("at_rev"), plan.at("at_rev")))
      throw Failure("revision_conflict", "The project changed; read it again");
    const auto full_plan =
        RawProjectCall(project, "run_plan", json::Encode(plan), stop);
    const auto answer = json::Object(full_plan);
    if (json::Text(answer, "reply") == "Failure")
      return policy.Filter(full_plan);
    policy.CheckContent(full_plan);
  }
  auto result = UnfilteredProjectCall(project, name, json::Encode(args), stop);
  if (name == "run_history" ||
      (name == "run_status" && !args.contains("run_id"))) {
    auto envelope = json::Object(result);
    const auto reply = json::Text(envelope, "reply");
    auto value = json::Object(json::Get(envelope, "value", "{}"));
    if (name == "run_history" && reply == "RunPage") {
      auto runs = json::Array(json::Get(value, "runs", "[]"));
      std::erase_if(runs, [&](const std::string& run) {
        const auto summary = json::Object(run);
        const auto status = json::Object(summary.at("status"));
        try {
          check_recorded(json::Text(status, "run_id"), "");
          return false;
        } catch (const Failure& error) {
          if (error.code != "not_found") throw;
          return true;
        }
      });
      value["runs"] = json::Encode(runs);
      envelope["value"] = json::Encode(value);
      result = json::Encode(envelope);
    } else if (name == "run_status" && reply == "RunStatus" &&
               value.contains("run_id")) {
      check_recorded(json::Text(value, "run_id"), "");
    }
  }
  const auto file_path = json::Text(args, "path");
  const bool project_document =
      file_path == "tsugumi.json" ||
      host::IsExternalDocument(file_path, "tsugumi.json");
  // layout.bin is read as its JSON view (the notes' text is in notes/).
  const bool layout_document =
      file_path == "layout.bin" ||
      host::IsExternalDocument(file_path, "layout.bin");
  if (name == "project_file_read" && (project_document || layout_document)) {
    // A projected document is a complete page. Guards retain the original hash.
    auto envelope = json::Object(result);
    if (json::Text(envelope, "reply") == "FileContent") {
      if (json::UInt(json::Get(args, "offset", "0")) != 0)
        throw Failure("invalid_request",
                      "Read the project document in one page");
      auto value = json::Object(json::Get(envelope, "value", "{}"));
      if (!json::Bool(json::Get(value, "eof")))
        throw Failure("invalid_request",
                      "Read the project document in one page");
      const auto bytes = host::DecodeSourceBytes(json::Get(value, "bytes", "\"\""));
      const auto text = policy.FileText(
          file_path, layout_document ? host::LayoutView(bytes, {}) : bytes);
      value["bytes"] = json::Quote(text);
      value["size_bytes"] = value["next_offset"] =
          json::Quote(std::to_string(text.size()));
      envelope["value"] = json::Encode(value);
      result = json::Encode(envelope);
    }
  }
  return policy.Filter(result, "", recorded);
}

auto Tools::UnfilteredProjectCall(const std::string& project,
                                  const std::string& name,
                                  const std::string& arguments,
                                  std::stop_token stop) -> std::string {
  auto args = json::Object(arguments);
  if (name == "input_types")
    return catalog.HostReply(
        wire::HostReply(1, InputTypesQuery(project, std::move(args), stop)));
  if (name == "project_save") {
    if (args.contains("transfers") &&
        !json::Array(args.at("transfers")).empty())
      throw Failure("permission_denied", "Use project_copy for file transfers");
    return SaveProject(project, std::move(args), stop);
  }
  if (name == "project_file_format") {
    H::FileFormatRequestT request;
    request.path = json::Text(args, "path");
    request.text = json::Text(args, "text");
    auto formatted = host::Format(request);
    std::vector<std::string> diagnostics;
    for (const auto& d : formatted.diagnostics) {
      json::Members diag{{"severity", json::Quote("error")},
                         {"code", json::Quote(d->code)},
                         {"message", json::Quote(d->message)},
                         {"path", json::Quote(d->path)}};
      if (d->offset) diag["offset"] = json::Quote(std::to_string(*d->offset));
      diagnostics.push_back(json::Encode(diag));
    }
    return json::Encode(json::Members{
        {"reply", json::Quote("FileFormatResult")},
        {"value", json::Encode(json::Members{
                      {"text", json::Quote(formatted.text)},
                      {"diagnostics", json::Encode(diagnostics)}})}});
  }
  const bool english = json::Text(args, "language") == "en";
  if (name == "version_graph") {
    args.erase("language");
    args["include_comments"] = "true";
  }
  if (name == "card_id") {
    args["stem"] = json::Quote(host::CardStem(json::Text(args, "name")));
    args.erase("name");
  }
  host::RestrictAgentRequest(name, args);
  std::string raw_reply;
  auto result =
      RawProjectCall(project, name, json::Encode(args), stop, &raw_reply);
  if (name == "project_file_read" || name == "project_file_list" ||
      name == "project_file_stat") {
    auto envelope = json::Object(result);
    auto failure = json::Object(json::Get(envelope, "value", "{}"));
    if (json::Text(envelope, "reply") == "Failure" &&
        json::Text(failure, "kind") == "not_found") {
      // This is advice from the host, not execution semantics in the core.
      // A failed inspection must not replace the original typed file failure.
      try {
        const auto read = [&](const std::string& method,
                              const json::Members& query) {
          const auto reply = json::Object(
              RawProjectCall(project, method, json::Encode(query), stop));
          const auto value = json::Object(json::Get(reply, "value", "{}"));
          if (json::Text(reply, "reply") == "Failure")
            throw Failure(json::Text(value, "kind"),
                          json::Text(value, "diagnostic"));
          return value;
        };
        const auto snapshot = read("project_inspect", {});
        const auto number = [](const std::string& raw) {
          return json::UInt(json::String(raw, raw));
        };
        if (!args.contains("at_rev") ||
            number(args.at("at_rev")) == number(json::Get(snapshot, "rev"))) {
          const auto supplement =
              host::DescribeFileMiss(json::Text(args, "path"), snapshot, read);
          if (!supplement.empty()) {
            failure["diagnostic"] = json::Quote(
                json::Text(failure, "diagnostic") + "; " + supplement);
            envelope["value"] = json::Encode(failure);
            result = json::Encode(envelope);
          }
        }
      } catch (const std::exception&) {
        if (stop.stop_requested()) throw Cancelled();
      }
    }
  }
  if (name == "version_graph") {
    auto envelope = json::Object(result);
    if (json::Text(envelope, "reply") == "VersionGraph") {
      const auto* reply = R::GetMessage(raw_reply.data())->payload_as_Reply();
      auto typed = std::unique_ptr<R::VersionGraphT>(
          reply->value_as_VersionGraph()->UnPack());
      host::PresentGraph(*typed, english);
      flatbuffers::FlatBufferBuilder builder;
      const auto value = R::VersionGraph::Pack(builder, typed.get());
      const auto response =
          R::CreateReply(builder, R::ReplyPayload::VersionGraph, value.Union());
      R::FinishMessageBuffer(
          builder,
          R::CreateMessage(builder, 1, R::Payload::Reply, response.Union()));
      result = catalog.Reply(
          {reinterpret_cast<const char*>(builder.GetBufferPointer()),
           builder.GetSize()});
    }
  }
  return result;
}

auto Tools::RawProjectCall(const std::string& project, const std::string& name,
                           const std::string& arguments, std::stop_token stop,
                           std::string* raw) -> std::string {
  auto prepared = arguments;
  uint64_t wait_seconds = 0;
  if (name == "run" || name == "run_status") {
    auto fields = json::Object(prepared);
    if (fields.contains("wait_seconds")) {
      wait_seconds = json::UInt(fields.at("wait_seconds"), UINT64_MAX);
      if (wait_seconds == UINT64_MAX)
        throw Failure("invalid_request",
                      "wait_seconds must be a nonnegative integer");
      // This budget is for the caller’s observation; the run can continue.
      wait_seconds = std::min<uint64_t>(wait_seconds, 300);
      fields.erase("wait_seconds");
      prepared = json::Encode(fields);
    }
  }
  Project endpoint;
  {
    std::lock_guard lock(mutex);
    endpoint = registry.Lookup(project);
  }
  client::Options transport_options;
  transport_options.endpoint = endpoint.endpoint;
  // This state belongs to this connection, never to the host's request lock.
  // Subscribe before querying/starting so a fast completion cannot be missed.
  struct RunWait {
    std::mutex mutex;
    std::condition_variable_any changed;
    std::map<std::string, std::unique_ptr<R::RunStatusT>> ended;
    bool closed = false;
  };
  std::shared_ptr<RunWait> waiting;
  client::Connection connection(transport_options);
  if (wait_seconds) {
    waiting = std::make_shared<RunWait>();
    connection.OnClosed([waiting](client::Error) {
      std::lock_guard lock(waiting->mutex);
      waiting->closed = true;
      waiting->changed.notify_all();
    });
    connection.OnEvent([waiting](client::Bytes bytes) {
      const auto* note = R::GetMessage(bytes.data())->payload_as_Notification();
      const auto* event = note ? note->value_as_RunEvent() : nullptr;
      if (!event) return;
      const R::RunStatus* status = nullptr;
      if (const auto* changed = event->value_as_RunStatusChanged())
        status = changed->status();
      else if (const auto* finished = event->value_as_RunFinished())
        status = finished->summary()->status();
      if (!status || !status->ended_at()) return;
      std::lock_guard lock(waiting->mutex);
      waiting->ended[event->run_id()->str()].reset(status->UnPack());
      waiting->changed.notify_all();
    });
  }
  std::stop_callback cancelled(stop, [&connection] { connection.Close(); });
  if (stop.stop_requested()) throw Cancelled();
  // Cancelling closes the connection, also while it is still being set up:
  // that is a cancelled call, not an unreachable project.
  if (auto error = connection.Start().get()) {
    if (stop.stop_requested()) throw Cancelled();
    throw Failure("unavailable", error.message);
  }
  auto hello =
      connection
          .Call([&](uint64_t id) {
            flatbuffers::FlatBufferBuilder b;
            auto h = R::CreateSessionHelloRequest(
                b, b.CreateString(project),
                endpoint.token.empty() ? 0 : b.CreateString(endpoint.token),
                Tsugumi::Trace::Client::Host);
            auto r = R::CreateRequest(b, R::RequestPayload::SessionHelloRequest,
                                      h.Union());
            R::FinishMessageBuffer(
                b, R::CreateMessage(b, id, R::Payload::Request, r.Union()));
            return client::Bytes(b.GetBufferPointer(),
                                 b.GetBufferPointer() + b.GetSize());
          })
          .get();
  if (stop.stop_requested()) throw Cancelled();
  if (hello.error) throw Failure("unavailable", hello.error.message);
  const auto* reply = R::GetMessage(hello.bytes.data())->payload_as_Reply();
  if (!reply || reply->value_type() != R::ReplyPayload::HelloResult)
    throw Failure("unauthenticated", "Project rejected the Agent host");
  auto query = [&](const std::string& tool, const std::string& args) {
    auto response = connection
                        .Call([&](uint64_t id) {
                          auto bytes = EncodeProjectRequest(tool, args, id);
                          return client::Bytes(bytes.begin(), bytes.end());
                        })
                        .get();
    if (stop.stop_requested()) throw Cancelled();
    if (response.error)
      throw Failure("unavailable", "Project connection lost while waiting: " +
                                       response.error.message);
    return response;
  };
  if (wait_seconds) {
    const auto opened = query("project_open", "{}");
    const auto* response =
        R::GetMessage(opened.bytes.data())->payload_as_Reply();
    if (!response || !response->value_as_OpenResult())
      throw Failure("unavailable",
                    "Could not subscribe to project run changes");
  }
  if (name == "project_validate" &&
      json::Object(arguments).contains("changes")) {
    try {
      prepared = PrepareLayoutSave(project, arguments, stop);
    } catch (const host::LayoutFailure& error) {
      return LayoutToolFailure("invalid_document", error.what());
    }
  }
  auto result = connection
                    .Call([&](uint64_t id) {
                      auto bytes = EncodeProjectRequest(name, prepared, id);
                      return client::Bytes(bytes.begin(), bytes.end());
                    })
                    .get();
  if (stop.stop_requested()) throw Cancelled();
  // Only a call that changes something can leave its outcome unknown.
  if (result.error && name == "run_status")
    throw Failure("unavailable", "Project connection lost while waiting: " +
                                     result.error.message);
  if (result.error) throw Failure("outcome_unknown", result.error.message);
  if (raw)
    raw->assign(reinterpret_cast<const char*>(result.bytes.data()),
                result.bytes.size());
  auto output =
      catalog.Reply({reinterpret_cast<const char*>(result.bytes.data()),
                     result.bytes.size()});
  if (!wait_seconds) return output;
  auto envelope = json::Object(output);
  if (json::Text(envelope, "reply") == "Failure") return output;
  auto value = json::Object(json::Get(envelope, "value", "{}"));
  auto status =
      name == "run" ? json::Object(json::Get(value, "run", "{}")) : value;
  const auto run_id = json::Text(status, "run_id");
  auto active = [&] {
    const auto state = json::Text(status, "state");
    return state == "queued" || state == "preparing" || state == "running" ||
           state == "cancelling";
  };
  std::string reason = run_id.empty() ? "idle" : "ended";
  if (active()) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(wait_seconds);
    std::unique_lock lock(waiting->mutex);
    waiting->changed.wait_until(lock, stop, deadline, [&] {
      return waiting->closed || waiting->ended.contains(run_id);
    });
    if (stop.stop_requested()) throw Cancelled();
    const auto found = waiting->ended.find(run_id);
    if (found != waiting->ended.end()) {
      flatbuffers::FlatBufferBuilder b;
      auto reply =
          R::CreateReply(b, R::ReplyPayload::RunStatus,
                         R::RunStatus::Pack(b, found->second.get()).Union());
      R::FinishMessageBuffer(
          b, R::CreateMessage(b, 1, R::Payload::Reply, reply.Union()));
      status = json::Object(
          json::Get(json::Object(catalog.Reply(
                        {reinterpret_cast<const char*>(b.GetBufferPointer()),
                         b.GetSize()})),
                    "value"));
    } else {
      if (waiting->closed)
        throw Failure("unavailable",
                      "Project connection lost while waiting for the run; the "
                      "core may have stopped or restarted");
      lock.unlock();
      // One read at the deadline supplies the current intermediate state.
      const auto current =
          query("run_status",
                json::Encode(json::Members{{"run_id", json::Quote(run_id)}}));
      const auto current_output =
          catalog.Reply({reinterpret_cast<const char*>(current.bytes.data()),
                         current.bytes.size()});
      const auto current_envelope = json::Object(current_output);
      if (json::Text(current_envelope, "reply") == "Failure")
        return current_output;
      status = json::Object(json::Get(current_envelope, "value"));
      reason = active() ? "timeout" : "ended";
    }
  }
  if (name == "run") {
    value["run"] = json::Encode(status);
    envelope["value"] = json::Encode(value);
  } else {
    envelope["value"] = json::Encode(status);
  }
  envelope["still_running"] = active() ? "true" : "false";
  envelope["wait_reason"] = json::Quote(reason);
  return json::Encode(envelope);
}

auto Tools::ApproveOperation(const std::string& project,
                             const std::string& conversation,
                             const ToolCall& call, const std::string& digest,
                             const json::Members& args, bool wait,
                             std::stop_token stop,
                             const std::vector<std::string>& permissions)
    -> std::vector<std::string> {
  std::vector<std::string> run_ids;
  const auto binding =
      permissions.empty()
          ? digest
          : crypto::sha256_hex(digest + "\n" + json::Encode(permissions));
  auto describe = [&] {
    if (!permissions.empty())
      return call.name + "\n" +
             json::Encode(
                 json::Members{{"project", json::Quote(project)},
                               {"arguments_digest", json::Quote(digest)},
                               {"permissions", json::Encode(permissions)}});
    return DescribeOperation(
        call.name, args, project,
        [&](const std::string& name, const json::Members& query) {
          const auto reply = json::Object(
              ProjectCall(project, name, json::Encode(query), stop));
          if (json::Text(reply, "reply") == "Failure")
            throw Failure(
                "approval_unavailable",
                "Cannot read approval targets: " + json::Encode(reply));
          return json::Object(json::Get(reply, "value", "{}"));
        },
        &run_ids);
  };
  std::unique_lock lock(mutex);
  auto approval = access_service.FindApproval(host::ApprovalKind::Operation,
                                              conversation, call.id, binding);
  if (!approval) {
    lock.unlock();
    const auto preview = describe();
    lock.lock();
    conversations->RememberResource(conversation, project);
    approval =
        access_service.RequestApproval(host::ApprovalKind::Operation, project,
                                       conversation, call.id, binding, preview);
  }
  access_service.WaitApproval(lock, approval->id, wait, stop);
  lock.unlock();
  const auto current = describe();
  lock.lock();
  const bool matches = current == approval->preview;
  auto consume = db.prepare(
      "UPDATE permission_requests SET state=? WHERE id=? AND state='allowed'");
  consume.bind(1, matches ? "consumed" : "stale").bind(2, approval->id).run();
  if (!db.changes())
    throw Failure("outcome_unknown", "This approval was already consumed");
  if (!matches)
    throw Failure("conflict", "Approval targets changed; propose a new call.");
  return run_ids;
}

auto Tools::ToolResources(const std::string& conversation,
                          const HelperScope* boundary)
    -> std::vector<std::string> {
  if (boundary && boundary->authorization())
    return {boundary->authorization()->project()};
  return conversations->Resources(conversation);
}

auto Tools::HelperReader(
    const std::string& conversation, std::stop_token stop,
    const std::string& revision,
    const std::optional<host::TurnAuthorization>& authorization,
    const host::SaveBase* inspected)
    -> HelperScope::Reader {
  std::string project;
  {
    std::lock_guard lock(mutex);
    project = conversations->GetConversation(conversation).project;
  }
  struct Context {
    std::optional<host::SaveBase> inspected;
    std::optional<host::AgentExclude> policy;
    std::map<std::string, std::string> files;
  };
  auto context = std::make_shared<Context>();
  return [this, project, conversation, stop, revision, authorization, inspected,
          context](const std::string& path) {
    access_service.Require(project, conversation,
                           Tsugumi::Service::Access::Read, true, stop,
                           "Read helper Card context", authorization);
    if (!inspected && !context->inspected)
      context->inspected = host::CaptureSaveBase(InspectProject(project, stop));
    const auto& base = inspected ? *inspected : *context->inspected;
    const auto at_rev = json::Get(base.snapshot, "rev");
    if (!revision.empty() && json::UInt(json::String(revision, revision)) !=
                                 json::UInt(json::String(at_rev, at_rev)))
      throw Failure("revision_conflict",
                    "The project changed; read the project again and "
                    "re-apply your input.");
    if (!context->policy) context->policy = ExclusionsFor(project, base);
    context->policy->Check(path);
    auto& cache = context->files;
    if (const auto found = cache.find(path); found != cache.end())
      return found->second;
    if (const auto found = base.files.find(path); found != base.files.end())
      return cache.emplace(path, context->policy->FileText(path, found->second))
          .first->second;
    json::Members args{{"path", json::Quote(path)},
                       {"limit_bytes", "1048576"},
                       {"at_rev", at_rev}};
    std::string bytes;
    for (;;) {
      args["offset"] = std::to_string(bytes.size());
      const auto reply = json::Object(RawProjectCall(
          project, "project_file_read", json::Encode(args), stop));
      if (json::Text(reply, "reply") == "Failure") {
        const auto failure = json::Object(json::Get(reply, "value", "{}"));
        const auto kind = json::Text(failure, "kind");
        if (kind == "not_found") {
          cache.emplace(path, "");
          return std::string{};
        }
        throw Failure(kind, json::Text(failure, "diagnostic"));
      }
      if (json::Text(reply, "reply") != "FileContent")
        throw Failure("internal_error", "Expected a helper file page");
      const auto value = json::Object(json::Get(reply, "value", "{}"));
      // Catalog replies expose byte vectors as UTF-8 text or base64 objects.
      const auto raw = json::Get(value, "bytes");
      std::string page;
      if (raw.starts_with('"'))
        page = json::String(raw);
      else if (!crypto::base64_decode(json::Text(json::Object(raw), "base64"),
                                      page))
        throw Failure("unavailable", "Invalid helper Card metadata encoding");
      bytes += page;
      if (json::Bool(json::Get(value, "eof"))) break;
      if (page.empty())
        throw Failure("unavailable",
                      "Helper scope metadata read made no progress");
    }
    cache.emplace(path, bytes);
    return bytes;
  };
}

auto Tools::ScopedResult(HelperScope& scope, const ToolCall& requested_call,
                         ToolResult result, const HelperScope::Reader& read)
    -> ToolResult {
  const auto call = catalog.ResolveCall(
      requested_call,
      scope.scoped() ? ModelRole::ScopedHelper : ModelRole::Helper);
  if (!scope.scoped() || call.name == "tool_result_read") return result;
  if (result.is_error) {
    if (catalog.Find(call.name))
      result.text = scope.FailureResult(result.text, read);
    return result;
  }
  scope.Completed(call.name, json::Object(call.arguments), result.text, read);
  if (call.name == "project_inspect")
    result.text = scope.Inspect(result.text, read);
  else if (call.name == "project_save" || call.name == "project_edit" ||
           call.name == "project_connect" ||
           call.name == "project_make_subgraph" ||
           call.name == "project_layout")
    result.text = scope.SaveResult(result.text, read);
  else if (call.name == "project_file_read") {
    const auto args = json::Object(call.arguments);
    const auto path = json::Text(args, "path");
    if (path == "tsugumi.json" || path.ends_with("/tsugumi.json")) {
      auto reply = json::Object(result.text);
      auto value = json::Object(json::Get(reply, "value", "{}"));
      // The projection is one complete document, never a slice of a full one.
      if (json::UInt(json::Get(args, "offset", "0")) != 0 ||
          !json::Bool(json::Get(value, "eof")))
        throw Failure("invalid_request",
                      "Read the scoped project document in one request");
      const auto projected = scope.Graph(json::Text(value, "bytes"),
                                         scope.ProjectPrefix(path, read));
      value["bytes"] = json::Quote(projected);
      value["next_offset"] = json::Quote(std::to_string(projected.size()));
      reply["value"] = json::Encode(value);
      result.text = json::Encode(reply);
    } else
      scope.CheckResult(result.text, read);
  } else if (catalog.Find(call.name) && call.name != "project_copy" &&
             call.name != "project_shell")
    scope.CheckResult(result.text, read);
  return result;
}

auto Tools::RequireHelperPermission(const HelperScope& boundary) -> void {
  if (!boundary.scoped() && !host_settings->AllowUnscopedHelpers())
    throw Failure("permission_denied",
                  "Unscoped helpers are disabled; ask the parent for "
                  "explicit read_cards, write_cards and run_cards");
}

auto Tools::ExecuteTool(const std::string& conversation,
                        const std::string& requested_project,
                        const ToolCall& requested_call, bool wait,
                        std::stop_token stop, bool helper,
                        HelperScope* boundary) -> ToolResult {
  Conversation c;
  {
    std::lock_guard lock(mutex);
    c = conversations->GetConversation(conversation);
  }
  const auto project =
      requested_project.empty() ? c.project : requested_project;
  const auto role = c.role == "guide"                ? ModelRole::Guide
                    : boundary && boundary->scoped() ? ModelRole::ScopedHelper
                    : helper                         ? ModelRole::Helper
                                                     : ModelRole::Main;
  const auto call = catalog.ResolveCall(requested_call, role);
  if (c.role == "guide" && call.name != "tools" &&
      !catalog.Allows(call.name, role))
    throw Failure("permission_denied", "Tool unavailable for the guide role");
  auto args = json::Object(call.arguments);
  if (call.name == "tools") {
    const auto family = json::Text(args, "family");
    std::vector<std::string> specs;
    for (const auto& spec : catalog.FamilySpecs(family, role))
      specs.push_back(json::Encode(
          json::Members{{"action", json::Quote(spec.name)},
                        {"description", json::Quote(spec.description)},
                        {"input_schema", spec.input_schema}}));
    return {json::Encode(specs), false, {}, {}};
  }
  if (call.name == "tool_result_read") {
    std::lock_guard lock(mutex);
    const auto wanted = json::Text(args, "call_id");
    std::vector<Message> saved;
    if (helper) {
      // A helper can reread only its own already-filtered transcript. Raw
      // project receipts contain fields outside a helper's Card boundary.
      auto q = conversations->ConversationDatabase(conversation)
                   .prepare(
                       "SELECT parent_call,task,messages,archive FROM "
                       "helpers WHERE conversation=?");
      q.bind(1, conversation);
      while (q.step()) {
        const auto scope = q.text(0) + "." + std::to_string(q.i64(1)) + ".";
        if (!call.id.starts_with(scope)) continue;
        saved = ParseMessages(q.text(2));
        if (!q.text(3).empty()) {
          const auto trace = ParseMessages(q.text(3));
          saved.insert(saved.end(), trace.begin(), trace.end());
        }
        break;
      }
    } else {
      saved = ParseMessages(c.messages);
      auto q = conversations->ConversationDatabase(conversation)
                   .prepare(
                       "SELECT messages FROM archives WHERE "
                       "conversation=? ORDER BY rowid DESC");
      q.bind(1, conversation);
      while (q.step()) {
        const auto trace = ParseMessages(q.text(0));
        saved.insert(saved.end(), trace.begin(), trace.end());
      }
    }
    for (const auto& message : saved) {
      for (const auto& block : message.content) {
        if (block.type != "tool_result" || block.id != wanted) continue;
        access_service.AuthorizeContext({message});
        const auto offset = json::UInt(json::Get(args, "offset"),
                                       json::UInt(json::Text(args, "offset")));
        if (offset > block.text.size())
          throw Failure("invalid_request", "Result offset exceeds total_bytes");
        const auto limit = std::clamp(
            json::UInt(json::Get(args, "limit_bytes"), kResultPageBytes),
            uint64_t{1}, uint64_t{kResultPageBytes});
        auto begin = static_cast<size_t>(offset);
        // Offsets refer to original UTF-8 bytes. Include a crossing character
        // on this page rather than returning invalid JSON text.
        while (begin && begin < block.text.size() &&
               (static_cast<unsigned char>(block.text[begin]) & 0xc0) == 0x80)
          --begin;
        auto end =
            std::min(block.text.size(), begin + static_cast<size_t>(limit));
        while (end < block.text.size() &&
               (static_cast<unsigned char>(block.text[end]) & 0xc0) == 0x80)
          ++end;
        return {
            json::Encode(json::Members{
                {"call_id", json::Quote(wanted)},
                {"offset", json::Quote(std::to_string(begin))},
                {"next_offset", json::Quote(std::to_string(end))},
                {"total_bytes", json::Quote(std::to_string(block.text.size()))},
                {"eof", end == block.text.size() ? "true" : "false"},
                {"text", json::Quote(block.text.substr(begin, end - begin))}}),
            false,
            {},
            block.resource_ids};
      }
    }
    throw Failure("not_found", "No saved tool result with this call_id");
  }
  // Model-facing defaults belong to the host. UI/core paging is unchanged.
  if (call.name == "run_value_read" || call.name == "artifact_read" ||
      call.name == "log_range") {
    if (!args.contains("limit_bytes"))
      args["limit_bytes"] = std::to_string(kResultPageBytes);
  }
  if (call.name == "run_history" || call.name == "run_invocations" ||
      call.name == "artifact_list" || call.name == "run_environments" ||
      call.name == "run_environment_paths") {
    if (!args.contains("limit")) args["limit"] = "8";
  }
  if (call.name == "demo_list" || call.name == "demo_read") {
    const auto policy = Exclusions(project, stop);
    return {call.name == "demo_list"
                ? demo_references.List(&policy)
                : demo_references.Read(json::Text(args, "path"), &policy),
            false,
            {},
            {}};
  }
  if (call.name == "guide_show") {
    if (c.role != "guide" || helper)
      throw Failure("permission_denied", "Only the guide can show guidance");
    dependencies.event(
        conversation,
        json::Encode(json::Members{{"kind", json::Quote("toolStarted")},
                                   {"id", json::Quote(call.id)},
                                   {"name", json::Quote(call.name)},
                                   {"input", json::Quote(call.arguments)}}));
    return {"Guidance sent to connected interfaces", false, {}, {}};
  }
  if (call.name == "delegate") {
    if (c.role == "guide") return {"Guide cannot delegate", true, {}, {}};
    if (boundary) RequireHelperPermission(*boundary);
    auto tasks = json::Array(json::Get(args, "tasks", "[]"));
    if (tasks.empty()) return {"Provide at least one task", true, {}, {}};
    const auto policy = host_settings->AllowUnscopedHelpers();
    std::set<std::string> names;
    for (const auto& task : tasks) {
      const auto spec = json::Object(task);
      try {
        HelperScope::Validate(spec, policy);
        if (boundary) boundary->CheckChild(spec);
      } catch (const std::exception& error) {
        throw Failure("invalid_request", error.what());
      }
      if (json::Text(spec, "name").size() > 128 ||
          json::Text(spec, "instruction").empty() ||
          json::Text(spec, "name").empty() ||
          !names.insert(json::Text(spec, "name")).second)
        throw Failure(
            "invalid_request",
            "Each helper needs a unique name and a nonempty instruction");
    }
    if (boundary) {
      std::lock_guard lock(mutex);
      auto& database = conversations->ConversationDatabase(conversation);
      sql::Transaction transaction(database);
      auto previous = database.prepare(
          "SELECT digest FROM helper_delegations WHERE "
          "conversation=? AND parent=? AND call=?");
      previous.bind(1, conversation)
          .bind(2, boundary->identity)
          .bind(3, call.id);
      const auto digest = crypto::sha256_hex(call.arguments);
      if (previous.step()) {
        if (previous.text(0) != digest)
          throw Failure("conflict",
                        "Delegation ID reused with different tasks");
      } else {
        auto used = database.prepare(
            "SELECT COALESCE(SUM(count),0) FROM helper_delegations WHERE "
            "conversation=? AND parent=?");
        used.bind(1, conversation).bind(2, boundary->identity);
        used.step();
        const auto total = used.u64(0);
        if (total > boundary->count() ||
            tasks.size() > boundary->count() - total)
          throw Failure("permission_denied",
                        "Helper lifetime child count exhausted");
        auto reserve = database.prepare(
            "INSERT INTO helper_delegations VALUES(?,?,?,?,?)");
        reserve.bind(1, conversation)
            .bind(2, boundary->identity)
            .bind(3, call.id)
            .bind(4, static_cast<uint64_t>(tasks.size()))
            .bind(5, digest)
            .run();
      }
      transaction.commit();
    }
    std::vector<std::future<std::string>> pending;
    for (size_t i = 0; i < tasks.size(); ++i) {
      if (stop.stop_requested()) throw Cancelled();
      pending.push_back(std::async(
          std::launch::async,
          [this, conversation, id = call.id, i, task = tasks[i], stop,
           authorization =
               boundary ? boundary->authorization() : std::nullopt] {
            return Helpers::HelperResult(
                dependencies.run_helper(conversation, id, i, task, stop,
                                        authorization),
                conversation, id, i);
          }));
    }
    std::vector<std::string> results;
    for (auto& future : pending) results.push_back(future.get());
    std::lock_guard lock(mutex);
    return {json::Encode(results),
            false,
            {},
            ToolResources(conversation, boundary)};
  }
  const auto* spec = catalog.Find(call.name);
  if (!spec) throw Failure("not_found", "Unknown tool");
  if (project.empty())
    throw Failure("invalid_request",
                  "Select a project before using project tools");
  const auto authorization =
      boundary ? boundary->authorization() : std::nullopt;
  access_service.Require(project, conversation, Tsugumi::Service::Access::Read,
                         wait, stop, "Read project content", authorization);
  if (boundary) {
    RequireHelperPermission(*boundary);
    std::optional<host::SaveBase> inspected;
    std::string revision = json::Get(args, "base_rev", "");
    if (boundary->scoped() &&
        (call.name == "project_file_list" || call.name == "project_file_read" ||
         call.name == "project_file_stat" || call.name == "project_validate" ||
         call.name == "run_plan" || call.name == "evaluate_nix")) {
      revision = json::Get(args, "at_rev", "");
      if (revision.empty()) {
        inspected = host::CaptureSaveBase(InspectProject(project, stop));
        revision = json::Get(inspected->snapshot, "rev");
      }
      args["at_rev"] = revision;
    }
    // Resolve nested file ownership and read the file at one revision. If
    // the parent changes a container link in between, the project rejects
    // the stale at_rev/base_rev instead of reading a different Card.
    const auto read = HelperReader(conversation, stop, revision, authorization,
                                   inspected ? &*inspected : nullptr);
    if (call.name != "project_edit" && call.name != "project_connect" &&
        call.name != "project_make_subgraph" && call.name != "project_layout")
      boundary->Check(call.name, args, read);
    if (boundary->scoped() && call.name == "evaluate_nix") {
      const auto snapshot = InspectProject(project, stop);
      const auto rev = json::Get(snapshot, "rev");
      if (json::String(rev, rev) != json::String(revision, revision))
        throw Failure("revision_conflict",
                      "The project changed; read it again");
      for (const auto& card :
           EvaluationCards(snapshot, json::Get(args, "card", "{}")))
        boundary->Check(
            "input_types",
            {{"card", json::Encode(json::Members{{"segments", card}})}}, read);
    }
    if (boundary->scoped() && (call.name == "run" || call.name == "run_plan" ||
                               call.name == "evaluate_nix")) {
      json::Members plan{{"card", json::Get(args, "card")}};
      if (call.name == "evaluate_nix")
        plan["propagate"] = "false";
      else if (args.contains("propagate"))
        plan["propagate"] = args.at("propagate");
      if (args.contains("base_rev"))
        plan["at_rev"] = args.at("base_rev");
      else if (args.contains("at_rev"))
        plan["at_rev"] = args.at("at_rev");
      const auto result =
          ProjectCall(project, "run_plan", json::Encode(plan), stop);
      boundary->CheckPlan(
          result, call.name == "run" || (call.name == "evaluate_nix" &&
                                         json::Bool(json::Get(args, "build"))));
      // A run must supply the same base_rev used by this plan. The project
      // rejects a missing or stale revision before executing anything.
    }
  }
  const bool execution =
      call.name == "run" || call.name == "run_cancel" ||
      (call.name == "evaluate_nix" && json::Bool(json::Get(args, "build"))) ||
      call.name == "project_shell";
  const bool changes_project =
      (!execution && (spec->mutating || spec->approval_required)) ||
      call.name == "card_id";
  if (changes_project || call.name == "project_shell")
    access_service.Require(project, conversation,
                           Tsugumi::Service::Access::Write, wait, stop,
                           "Change project content", authorization);
  if (execution)
    access_service.Require(project, conversation, Tsugumi::Service::Access::Run,
                           wait, stop, "Execute or cancel project work",
                           authorization);
  if (c.role == "guide" && (changes_project || execution))
    throw Failure("permission_denied", "The guide cannot change projects");
  if (call.name == "project_shell") {
    const bool english =
        host_settings->Language() == "en";
    const std::string reason =
        english
            ? "Execute shell commands as the project user in its environment. "
              "Commands can change writable Card source files through a "
              "validated save. "
              "Attached folders and structural documents are read-only. "
              "Command: "
            : "プロジェクトの実行環境でシェルコマンドを実行します。"
              "書き込みが許可された Card "
              "のソースを編集し、検証して保存できます。"
              "添付フォルダと構造文書は読み取り専用です。コマンド: ";
    access_service.Require(project, conversation,
                           Tsugumi::Service::Access::Shell, wait, stop,
                           reason + json::Text(args, "command"));
  }
  std::string folder_resource;
  if (call.name == "attached_folder_read" ||
      (call.name == "project_copy" &&
       json::Text(args, "source") == "attached")) {
    Exclusions(project, stop).Check(json::Text(args, "folder"));
    {
      std::lock_guard lock(mutex);
      auto folder =
          db.prepare("SELECT id FROM folders WHERE project=? AND path=?");
      folder.bind(1, project).bind(2, json::Text(args, "folder"));
      if (!folder.step())
        throw Failure("permission_denied",
                      "The user has not registered this attached folder");
      folder_resource = folder.text(0);
    }
    access_service.Require(folder_resource, conversation,
                           Tsugumi::Service::Access::Read, wait, stop,
                           "Read the user-selected attached folder");
  }
  host::StripOwnedFields(call.name, spec->mutating, args);
  if (call.name == "evaluate_nix") {
    // Repeated evaluations use the confirmed revision without a durable tool receipt.
    auto result = ProjectCall(project, call.name, json::Encode(args), stop);
    const auto envelope = json::Object(result);
    const auto value = json::Object(json::Get(envelope, "value", "{}"));
    const bool error = json::Text(envelope, "reply") == "Failure" ||
                       json::Get(value, "failure", "null") != "null";
    {
      std::lock_guard lock(mutex);
      conversations->RememberResource(conversation, project);
    }
    return ToolOutput(result, error, {project});
  }
  const auto digest = crypto::sha256_hex(call.name + "\n" + project + "\n" +
                                         json::Encode(args));
  auto op_id = conversation + "-" + call.id;
  if (spec->mutating) {
    if (spec->has_origin) args["origin"] = json::Quote("agent");
    args["op_id"] = json::Quote(op_id);
  }
  bool interrupted = false;
  {
    std::lock_guard lock(mutex);
    auto old = conversations->ConversationDatabase(conversation)
                   .prepare(
                       "SELECT digest,state,result,error,operation FROM "
                       "receipts WHERE conversation=? AND id=?");
    old.bind(1, conversation).bind(2, call.id);
    if (old.step()) {
      if (old.text(0) != digest)
        throw Failure("conflict",
                      "The operation ID was reused for different arguments");
      if (old.text(1) == "completed")
        return ToolOutput(old.text(2), old.i64(3) != 0,
                          ToolResources(conversation, boundary));
      op_id = old.text(4);
      interrupted = true;
      if (!spec->approval_required) {
        auto approval = db.prepare(
            "SELECT state,id,reason FROM permission_requests "
            "WHERE conversation=? AND call=? AND access=?");
        approval.bind(1, conversation)
            .bind(2, call.id)
            .bind(3, static_cast<int>(host::ApprovalKind::Operation));
        if (approval.step()) {
          const auto state = approval.text(0);
          if (state == "pending" && !wait)
            throw Failure("grant_required", approval.text(2), approval.text(1));
          if (state == "denied")
            throw Failure("permission_denied",
                          "The user declined this operation. Nothing was "
                          "changed; continue without retrying it.");
          // The core rejected this attempt before asking for Card permissions;
          // approval_required has no durable core receipt. Recheck the same
          // unapproved request, including reload's current on-disk widening.
          // ApproveOperation compares its exact binding before consuming it.
          // Once consumed, only receipt recovery may resolve the operation.
          if (state == "pending" || state == "allowed") interrupted = false;
        }
      }
    } else if (!spec->approval_required) {
      auto q =
          conversations->ConversationDatabase(conversation)
              .prepare("INSERT INTO receipts VALUES(?,?,?,'sent','',0,?,?)");
      q.bind(1, conversation)
          .bind(2, call.id)
          .bind(3, digest)
          .bind(4, project)
          .bind(5, op_id)
          .run();
    }
  }
  if (spec->approval_required && !interrupted) {
    const auto run_ids = ApproveOperation(project, conversation, call, digest,
                                          args, wait, stop, {});
    if (call.name == "run_history_delete") {
      args["run_ids"] = json::Encode(run_ids);
      args["restrict_to_run_ids"] = "true";
    }

    std::lock_guard lock(mutex);
    auto q = conversations->ConversationDatabase(conversation)
                 .prepare("INSERT INTO receipts VALUES(?,?,?,'sent','',0,?,?)");
    q.bind(1, conversation)
        .bind(2, call.id)
        .bind(3, digest)
        .bind(4, project)
        .bind(5, op_id)
        .run();
  }
  // Keep the original scoped request across permission approval. Expansion and
  // binding checks are identical on the initial send and after approval.
  auto project_call = [&]() {
    if (!boundary || !boundary->scoped() || call.name != "project_save")
      return ProjectCall(
          project,
          (call.name == "project_copy" || call.name == "project_shell")
              ? "project_save"
              : call.name,
          json::Encode(args), stop);
    try {
      const auto before = HelperReader(
          conversation, stop, json::Get(args, "base_rev"), authorization);
      const auto expanded = boundary->ExpandSave(args, before);
      return SaveProject(
          project, expanded, stop, [&](const host::SaveBase& latest) {
            boundary->CheckBindings(before, [&](const std::string& path) {
              const auto found = latest.files.find(path);
              return found == latest.files.end() ? std::string{}
                                                 : found->second;
            });
          });
    } catch (const Failure& failure) {
      if (failure.code != "revision_conflict") throw;
      return LayoutToolFailure("revision_conflict", failure.what());
    }
  };
  // An interrupted mutation can only be resolved by its durable receipt.
  // Reissuing it after a daemon restart may execute it a second time.
  std::string result;
  std::vector<std::string> file_resources;
  if (call.name == "template_list") {
    const auto items = ListTemplates(project, true, stop);
    std::vector<std::string> entries;
    for (const auto& item : items.entries)
      entries.push_back(json::Encode(json::Members{
          {"name", json::Quote(item->name)},
          {"store",
           json::Quote(item->store == H::TemplateStore::Project ? "project"
                                                                : "host")}}));
    result = json::Encode(json::Members{
        {"reply", json::Quote("TemplateList")},
        {"value",
         json::Encode(json::Members{{"entries", json::Encode(entries)}})}});
  } else if (call.name == "project_edit" || call.name == "project_connect" ||
             call.name == "project_make_subgraph" ||
             call.name == "project_layout" || call.name == "template_copy" ||
             call.name == "template_add" || call.name == "template_import") {
    result = EditProjectTool(conversation, project, call, args, op_id, boundary,
                             interrupted, stop);
  } else if (interrupted && spec->mutating) {
    result = ProjectCall(
        project, "operation_get",
        json::Encode(json::Members{{"op_id", json::Quote(op_id)}}), stop);
    auto value = json::Object(json::Get(json::Object(result), "value", "{}"));
    if (json::Text(value, "state") != "completed")
      throw Failure("outcome_unknown",
                    "The earlier operation has no terminal receipt");
    auto receipt = json::Object(json::Get(value, "receipt", "{}"));
    result = json::Encode(
        json::Members{{"reply", json::Get(receipt, "result_type")},
                      {"value", json::Get(receipt, "result", "{}")}});
    if (call.name == "run" && args.contains("wait_seconds") &&
        json::Get(args, "wait_seconds") != "0") {
      auto accepted = json::Object(result);
      auto value = json::Object(json::Get(accepted, "value", "{}"));
      const auto status = json::Object(json::Get(value, "run", "{}"));
      const auto waited = json::Object(
          ProjectCall(project, "run_status",
                      json::Encode(json::Members{
                          {"run_id", json::Get(status, "run_id")},
                          {"wait_seconds", json::Get(args, "wait_seconds")}}),
                      stop));
      if (json::Text(waited, "reply") == "Failure") {
        result = json::Encode(waited);
      } else {
        value["run"] = json::Get(waited, "value");
        accepted["value"] = json::Encode(value);
        for (const auto* field : {"still_running", "wait_reason"})
          accepted[field] = json::Get(waited, field);
        result = json::Encode(accepted);
      }
    }
  } else {
    if (stop.stop_requested()) throw Cancelled();
    if (call.name == "version_graph")
      args["language"] = json::Quote(
          host_settings->Language());
    if (call.name == "project_copy" || call.name == "project_shell") {
      result = FileOperation(conversation, project, call.name, args, op_id,
                             boundary, stop, file_resources);
    } else if (call.name == "attached_folder_read") {
      try {
        const auto policy = Exclusions(project, stop);
        result =
            host::ReadAttachedFolder(json::Text(args, "folder"), args, &policy);
      } catch (const host::FolderReadFailure& error) {
        throw Failure(error.code, error.what());
      }
    } else if (call.name == "version_diff") {
      // The project core applies the same allowlisted, size-limited diff as
      // Compare Runs.
      result = RawProjectCall(
          project, "version_diff",
          json::Encode(json::Members{
              {"commit", json::Quote(json::Text(args, "commit"))},
              {"from_commit", json::Quote(json::Text(args, "from_commit"))}}),
          stop);
    } else if (call.name == "version_find") {
      // Composes the same reads the Agent could make one by one, each with
      // its own exclusion checks.
      result = json::Encode(json::Members{
          {"reply", json::Quote("VersionFind")},
          {"value",
           FindVersions(args, [&](const std::string& name,
                                  const json::Members& query) {
             const auto reply = json::Object(
                 ProjectCall(project, name, json::Encode(query), stop));
             const auto value = json::Object(json::Get(reply, "value", "{}"));
             if (json::Text(reply, "reply") == "Failure")
               throw Failure(json::Text(value, "kind"),
                             json::Text(value, "diagnostic"));
             return value;
           })}});
    } else if (spec->host) {
      const auto bytes = catalog.HostRequest(call.name, json::Encode(args), 1);
      try {
        result = catalog.HostReply(host::LayoutHandle(
            *H::GetMessage(bytes.data())->payload_as_Request(), 1));
      } catch (const host::LayoutFailure& error) {
        result = LayoutToolFailure("invalid_request", error.what());
      }
    } else {
      result = project_call();
    }
  }
  if (call.name == "project_file_read") {
    auto page = json::Object(result);
    if (json::Text(page, "reply") == "FileContent") {
      auto value = json::Object(json::Get(page, "value", "{}"));
      value["hint"] = json::Quote(
          "Use project_copy to move files without reading them, or "
          "project_shell to examine large files. Reads default to "
          "256 KiB; maximum 1 MiB.");
      page["value"] = json::Encode(value);
      result = json::Encode(page);
    }
  }
  if (!UsesRecordedDefinitions(call.name))
    result = Exclusions(project, stop).Filter(result);
  const auto envelope = json::Object(result);
  bool error = json::Text(envelope, "reply") == "Failure";
  if (error) {
    auto failure = json::Object(json::Get(envelope, "value", "{}"));
    if (json::Text(failure, "kind") == "approval_required") {
      if (authorization)
        throw Failure(
            "permission_denied",
            "A scoped Skill cannot widen Card permissions at runtime");
      std::vector<std::string> permissions;
      for (const auto& arg : json::Array(json::Get(failure, "args", "[]"))) {
        auto pair = json::Object(arg);
        if (json::Text(pair, "key") == "permission")
          permissions.push_back(json::Get(pair, "value"));
      }
      std::sort(permissions.begin(), permissions.end());
      permissions.erase(std::unique(permissions.begin(), permissions.end()),
                        permissions.end());
      if (permissions.empty())
        throw Failure("approval_unavailable",
                      "The project reported no permissions to approve");
      ApproveOperation(project, conversation, call, digest, args, wait, stop,
                       permissions);
      args["approved_permissions"] = json::Encode(permissions);
      if (call.name == "project_copy" &&
          (json::Text(args, "source") == "demo" ||
           (json::Text(args, "source") == "attached" &&
            (args.contains("card") || args.contains("id"))))) {
        result = FileOperation(conversation, project, call.name, args, op_id,
                               boundary, stop, file_resources);
      } else if (call.name == "project_edit" ||
                 call.name == "project_connect" ||
                 call.name == "project_make_subgraph" ||
                 call.name == "project_layout" ||
                 call.name == "template_copy") {
        result = EditProjectTool(conversation, project, call, args, op_id,
                                 boundary, false, stop);
      } else {
        op_id += "-approved";
        args["op_id"] = json::Quote(op_id);
        {
          std::lock_guard lock(mutex);
          sql::Transaction tx(
              conversations->ConversationDatabase(conversation));
          auto receipt = conversations->ConversationDatabase(conversation)
                             .prepare(
                                 "UPDATE receipts SET operation=? WHERE "
                                 "conversation=? AND id=?");
          receipt.bind(1, op_id).bind(2, conversation).bind(3, call.id).run();
          tx.commit();
        }
        result = project_call();
      }
      error = json::Text(json::Object(result), "reply") == "Failure";
    }
  }
  if (call.name == "project_copy" || call.name == "project_shell") {
    // This also applies to receipt recovery. File operations expose only
    // their own source changes, never another Card's resolution/diagnostics.
    auto envelope = json::Object(result);
    auto value = json::Object(json::Get(envelope, "value", "{}"));
    auto compact_save = [](json::Members save) {
      if (save.contains("delta")) {
        auto delta = json::Object(save.at("delta"));
        delta.erase("resolution");
        delta.erase("diagnostics");
        save["delta"] = json::Encode(delta);
      }
      return save;
    };
    if (json::Text(envelope, "reply") == "SaveResult")
      value = compact_save(value);
    if (value.contains("save"))
      value["save"] =
          json::Encode(compact_save(json::Object(value.at("save"))));
    envelope["value"] = json::Encode(value);
    result = json::Encode(envelope);
  }
  if (boundary && boundary->scoped() && call.name == "card_id" && !error) {
    // Keep creation provenance with this helper's existing durable receipt,
    // rather than relying on a tool call remaining in compacted messages.
    auto owned = json::Object(result);
    owned["helper_project_path"] =
        json::Get(args, "project_path", json::Quote(""));
    result = json::Encode(owned);
  }
  if (boundary && boundary->scoped() && call.name == "run" && !error) {
    auto owned = json::Object(result);
    owned["helper_run_card"] = json::Get(args, "card");
    result = json::Encode(owned);
  }
  {
    std::lock_guard lock(mutex);
    conversations->RememberResource(conversation, project);
    if (!folder_resource.empty()) {
      conversations->RememberResource(conversation, folder_resource);
    }
    auto q = conversations->ConversationDatabase(conversation)
                 .prepare(
                     "UPDATE receipts SET state='completed',result=?,error=? "
                     "WHERE conversation=? AND id=?");
    q.bind(1, dependencies.scrub(result))
        .bind(2, error)
        .bind(3, conversation)
        .bind(4, call.id)
        .run();
  }
  if (spec->approval_required && !wait)
    dependencies.event(conversation, json::Encode(json::Members{
                                         {"kind", json::Quote("toolFinished")},
                                         {"id", json::Quote(call.id)},
                                         {"name", json::Quote(call.name)},
                                         {"text", json::Quote(result)},
                                         {"ok", error ? "false" : "true"}}));
  std::vector<std::string> resources{project};
  if (!folder_resource.empty()) resources.push_back(folder_resource);
  resources.insert(resources.end(), file_resources.begin(),
                   file_resources.end());
  return ToolOutput(result, error, std::move(resources));
}

auto Tools::AllocateCall(const std::string& conversation,
                         const std::string& scope) -> std::string {
  std::lock_guard lock(mutex);
  auto& store = conversations->ConversationDatabase(conversation);
  auto q = store.prepare(
      "INSERT INTO call_counters VALUES(?,?,1) ON "
      "CONFLICT(conversation,scope) DO UPDATE SET value=value+1 "
      "WHERE value<9223372036854775807 RETURNING value");
  q.bind(1, conversation).bind(2, scope);
  if (!q.step())
    throw Failure("persistence_failed", "Cannot allocate a tool call");
  return std::to_string(q.i64(0));
}

auto Tools::Hooks(const std::string& id, bool helper,
                  std::shared_ptr<HelperScope> boundary) -> RuntimeHooks {
  RuntimeHooks hooks;
  hooks.allocate_call = [this, id, boundary] {
    return AllocateCall(id, boundary ? boundary->identity : "");
  };
  hooks.authorize_context = [this, boundary](const auto& messages) {
    access_service.AuthorizeContext(
        messages, boundary ? boundary->authorization() : std::nullopt);
  };
  hooks.checkpoint = [this, id](const auto& messages) {
    std::lock_guard lock(mutex);
    conversations->SaveMessages(id, messages);
    dependencies.event(id, R"({"kind":"checkpoint"})");
  };
  hooks.execute_tool = [this, id, helper, boundary](const ToolCall& call,
                                                    std::stop_token stop) {
    try {
      auto result =
          ExecuteTool(id, {}, call, true, stop, helper, boundary.get());
      std::string revision;
      if (boundary && call.name == "project_inspect" && !result.is_error)
        revision = json::Get(
            json::Object(json::Get(json::Object(result.text), "value", "{}")),
            "rev", "");
      return boundary ? ScopedResult(*boundary, call, std::move(result),
                                     HelperReader(id, stop, revision,
                                                  boundary->authorization()))
                      : result;
    } catch (const Cancelled&) {
      throw;
    } catch (const Failure& error) {
      // Host invariant failures terminate the turn; the model cannot repair
      // them by changing its arguments or retrying the operation.
      if (error.code == "internal_error") throw;
      return ToolResult{
          LayoutToolFailure(error.code, error.what()), true, {}, {}};
    } catch (const std::exception& error) {
      return ToolResult{error.what(), true, {}, {}};
    }
  };
  hooks.event = [this, id](const RuntimeEvent& e) {
    if (e.kind == "toolStarted")
      turns->RecordOperation(id, "tool", e.id, e.name, e.text);
    else if (e.kind == "compacting")
      turns->RecordOperation(id, "compact");
    else if (e.kind == "toolFinished" || e.kind == "compacted")
      turns->RecordOperation(id, "model");
    if (e.kind == "finished") return;
    dependencies.event(
        id, json::Encode(json::Members{{"kind", json::Quote(e.kind)},
                                       {"text", json::Quote(e.text)},
                                       {"message", json::Quote(e.text)},
                                       {"code", json::Quote(e.code)},
                                       {"input", json::Quote(e.text)},
                                       {"id", json::Quote(e.id)},
                                       {"name", json::Quote(e.name)},
                                       {"ok", e.ok ? "true" : "false"},
                                       {"index", std::to_string(e.index)}}));
  };
  hooks.reconcile_tool =
      [this, id, helper,
       boundary](const ToolCall& requested_call) -> std::optional<ToolResult> {
    std::lock_guard lock(mutex);
    const auto role = conversations->GetConversation(id).role == "guide"
                          ? ModelRole::Guide
                      : boundary && boundary->scoped() ? ModelRole::ScopedHelper
                      : helper                         ? ModelRole::Helper
                                                       : ModelRole::Main;
    const auto call = catalog.ResolveCall(requested_call, role);
    if (call.name == "delegate") {
      if (!boundary || !boundary->authorization())
        conversations->AuthorizeHistory(id);
      auto helpers = conversations->ConversationDatabase(id).prepare(
          "SELECT result,state,task FROM helpers WHERE conversation=? "
          "AND parent_call=? ORDER BY task");
      helpers.bind(1, id).bind(2, call.id);
      std::vector<std::string> results;
      bool incomplete = false;
      while (helpers.step()) {
        incomplete |= helpers.text(1) != "completed";
        if (!helpers.text(0).empty())
          results.push_back(Helpers::HelperResult(helpers.text(0), id, call.id,
                                                  helpers.i64(2)));
      }
      const auto tasks =
          json::Array(json::Get(json::Object(call.arguments), "tasks", "[]"));
      incomplete |= results.size() != tasks.size();
      if (!incomplete)
        return ToolResult{json::Encode(results),
                          false,
                          {},
                          ToolResources(id, boundary.get())};
      return ToolResult{
          "Helpers were interrupted. Their saved messages and operation "
          "receipts remain in the conversation; inspect them before "
          "assigning more work. Completed results: " +
              json::Encode(results),
          true,
          {},
          ToolResources(id, boundary.get())};
    }
    auto q = conversations->ConversationDatabase(id).prepare(
        "SELECT state,result,error,project,operation FROM "
        "receipts WHERE conversation=? AND id=?");
    q.bind(1, id).bind(2, call.id);
    if (!q.step()) return std::nullopt;
    if (q.text(0) == "completed")
      return ToolOutput(q.text(1), q.i64(2) != 0,
                        ToolResources(id, boundary.get()));
    return ToolResult{
        "Operation outcome is unknown. Query operation_get with op_id=" +
            q.text(4) + " before continuing.",
        true,
        {},
        {q.text(3)}};
  };
  if (boundary) {
    const auto reconcile = hooks.reconcile_tool;
    hooks.reconcile_tool =
        [this, id, boundary, reconcile](
            const ToolCall& requested_call) -> std::optional<ToolResult> {
      try {
        RequireHelperPermission(*boundary);
        const auto call = catalog.ResolveCall(
            requested_call,
            boundary->scoped() ? ModelRole::ScopedHelper : ModelRole::Helper);
        if (call.name == "tools") return std::nullopt;
        if (call.name == "delegate") {
          // Delegate has no core receipt and is not a project tool. Match its
          // lifetime reservation before returning saved children; do not charge
          // the capability again or launch work during reconciliation.
          const auto tasks = json::Array(
              json::Get(json::Object(call.arguments), "tasks", "[]"));
          for (const auto& task : tasks)
            boundary->CheckChild(json::Object(task));
          std::lock_guard lock(mutex);
          auto reserved = conversations->ConversationDatabase(id).prepare(
              "SELECT digest,count FROM helper_delegations WHERE "
              "conversation=? AND parent=? AND call=?");
          reserved.bind(1, id).bind(2, boundary->identity).bind(3, call.id);
          if (!reserved.step()) return std::nullopt;
          if (reserved.text(0) != crypto::sha256_hex(call.arguments) ||
              reserved.u64(1) != tasks.size())
            throw Failure("conflict",
                          "Delegation ID reused with different tasks");
          return reconcile(call);
        }
        const auto read = HelperReader(id, shutdown.get_token(), {},
                                       boundary->authorization());
        boundary->Check(call.name, json::Object(call.arguments), read);
        auto result = reconcile(call);
        if (result) {
          std::string revision;
          if (call.name == "project_inspect" && !result->is_error)
            revision =
                json::Get(json::Object(json::Get(json::Object(result->text),
                                                 "value", "{}")),
                          "rev", "");
          return ScopedResult(*boundary, call, std::move(*result),
                              HelperReader(id, shutdown.get_token(), revision,
                                           boundary->authorization()));
        }
        return std::nullopt;
      } catch (const std::exception& error) {
        return ToolResult{error.what(), true, {}, {}};
      }
    };
  }
  return hooks;
}

auto Tools::Handle(const RequestIn& request, bool human,
                   std::unique_lock<std::recursive_mutex>& lock)
    -> std::string {
  const auto* r = A::GetMessage(request.frame->data())->payload_as_Request();
  switch (r->value_type()) {
    case A::RequestPayload::CallIdRequest: {
      const auto conversation =
          r->value_as_CallIdRequest()->conversation_id()->str();
      conversations->OwnConversation(conversation, human);
      A::CallIdT result;
      const auto* input = r->value_as_CallIdRequest();
      if (input->name() && input->arguments_json()) {
        const auto call = catalog.ResolveCall(
            {"", input->name()->str(), input->arguments_json()->str()},
            ModelRole::Main);
        if (const auto* spec = catalog.Find(call.name)) {
          auto args = json::Object(call.arguments);
          host::StripOwnedFields(call.name, spec->mutating, args);
          const auto project =
              conversations->GetConversation(conversation).project;
          const auto digest = crypto::sha256_hex(call.name + "\n" + project +
                                                 "\n" + json::Encode(args));
          // Explicitly guarded calls wait for approval before recording a
          // sent receipt. A retry must resume the approved call ID.
          if (spec->approval_required) {
            auto approval = db.prepare(
                "SELECT call FROM permission_requests WHERE conversation=? "
                "AND project=? AND access=? AND digest=? AND state "
                "IN ('pending','allowed') ORDER BY rowid DESC LIMIT 1");
            approval.bind(1, conversation)
                .bind(2, project)
                .bind(3, static_cast<int>(host::ApprovalKind::Operation))
                .bind(4, digest);
            if (approval.step()) result.id = approval.text(0);
          }
          if (!result.id.empty())
            return wire::AgentReply(request.request_id, std::move(result));
          auto receipts =
              conversations->ConversationDatabase(conversation)
                  .prepare(
                      "SELECT id FROM receipts WHERE conversation=? AND "
                      "digest=? AND state<>'completed' ORDER BY id DESC");
          receipts.bind(1, conversation).bind(2, digest);
          while (receipts.step()) {
            auto approval = db.prepare(
                "SELECT 1 FROM permission_requests WHERE conversation=? AND "
                "call=? "
                "AND access=? AND state "
                "IN ('pending','allowed')");
            approval.bind(1, conversation)
                .bind(2, receipts.text(0))
                .bind(3, static_cast<int>(host::ApprovalKind::Operation));
            if (approval.step()) {
              result.id = receipts.text(0);
              break;
            }
          }
        }
      }
      if (result.id.empty()) result.id = AllocateCall(conversation);
      return wire::AgentReply(request.request_id, std::move(result));
    }
    case A::RequestPayload::ToolCatalogRequest: {
      A::ToolCatalogT result;
      for (const auto& spec : Specs()) {
        auto tool = std::make_unique<A::ToolT>();
        tool->name = spec.name;
        tool->description = spec.description;
        tool->input_schema_json = spec.input_schema;
        result.tools.push_back(std::move(tool));
      }
      return wire::AgentReply(request.request_id, std::move(result));
    }
    case A::RequestPayload::ToolCallRequest: {
      auto p = r->value_as_ToolCallRequest();
      if (p->op_id()->size() == 0 || p->op_id()->size() > 256)
        throw Failure("invalid_request",
                      "A tool call needs a nonempty ID of at most 256 bytes");
      const auto id = p->conversation_id()->str(),
                 project = Text(p->project_id());
      conversations->OwnConversation(id, human);
      const auto call = catalog.ResolveCall(
          {p->op_id()->str(), p->name()->str(), p->arguments_json()->str()},
          conversations->GetConversation(id).role == "guide" ? ModelRole::Guide
                                                             : ModelRole::Main);
      std::shared_ptr<std::stop_source> shell_stop;
      const auto shell_key = std::make_pair(id, call.id);
      if (call.name == "project_shell" || call.name == "project_copy" ||
          call.name == "template_add" || call.name == "template_import" ||
          call.name == "run_status" || call.name == "run") {
        if (file_calls.contains(shell_key))
          throw Failure("busy", "This tool call is already running");
        shell_stop = std::make_shared<std::stop_source>();
        file_calls.emplace(
            shell_key, FileCall{project.empty()
                                    ? conversations->GetConversation(id).project
                                    : project,
                                shell_stop});
      }
      lock.unlock();
      // External plugins use the same conversation cancellation and visible
      // tool history as model-issued calls.
      auto execute = [&]() -> ToolResult {
        if (!shell_stop)
          return ExecuteTool(id, project, call, false, shutdown.get_token());
        struct Registration {
          Tools& tools;
          std::pair<std::string, std::string> key;
          std::shared_ptr<std::stop_source> stop;
          ~Registration() {
            std::lock_guard guard(tools.mutex);
            const auto found = tools.file_calls.find(key);
            if (found != tools.file_calls.end() && found->second.stop == stop)
              tools.file_calls.erase(found);
          }
        } registration{*this, shell_key, shell_stop};
        std::stop_callback stop(shutdown.get_token(),
                                [shell_stop] { shell_stop->request_stop(); });
        std::stop_callback disconnected(request.connection_stop, [shell_stop] {
          shell_stop->request_stop();
        });
        if (shell_stop->stop_requested()) throw Cancelled();
        auto event = [&](const char* kind, const std::string& text, bool ok) {
          std::lock_guard guard(mutex);
          const auto found = file_calls.find(shell_key);
          // Deletion stops and unregisters calls under this same lock. Never
          // record their late events in a new conversation reusing the ID.
          if (found == file_calls.end() || found->second.stop != shell_stop)
            return;
          dependencies.event(id, json::Encode(json::Members{
                                     {"kind", json::Quote(kind)},
                                     {"id", json::Quote(call.id)},
                                     {"name", json::Quote(call.name)},
                                     {"input", json::Quote(call.arguments)},
                                     {"text", json::Quote(text)},
                                     {"message", json::Quote(text)},
                                     {"ok", ok ? "true" : "false"}}));
        };
        event("toolStarted", call.arguments, true);
        try {
          auto result =
              ExecuteTool(id, project, call, false, shell_stop->get_token());
          event("toolFinished", result.text, !result.is_error);
          return result;
        } catch (const std::exception& error) {
          event("toolFinished", error.what(), false);
          throw;
        }
      };
      auto result = execute();
      A::ToolResultT reply;
      reply.text = result.text;
      reply.is_error = result.is_error;
      for (const auto& image : result.images) {
        auto item = std::make_unique<A::ImageT>();
        item->media_type = image.media_type;
        item->data = image.data;
        reply.images.push_back(std::move(item));
      }
      return wire::AgentReply(request.request_id, std::move(reply));
    }
    default:
      throw Failure("invalid_request", "Unsupported tool request");
  }
}
}  // namespace tsugumi::agent
