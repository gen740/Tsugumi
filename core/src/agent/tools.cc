#include "agent/tools.hh"

#include <algorithm>
#include <condition_variable>
#include <future>

#include "agent/context.hh"
#include "agent/providers.hh"
#include "agent/save_policy.hh"
#include "agent/tool_images.hh"
#include "agent/turns.hh"
#include "agent/wire.hh"
#include "host/access.hh"
#include "host/agent_policy.hh"
#include "host/attached_folder.hh"
#include "host/demo_references.hh"
#include "host/document_edits.hh"
#include "host/failure.hh"
#include "host/format.hh"
#include "host/layout_saving.hh"
#include "host/layout_service.hh"
#include "host/save.hh"
#include "host/settings.hh"
#include "host/templates.hh"
#include "host/version_text.hh"
#include "tsugumi/client/client.hh"
#include "tsugumi/protocol/tsugumi_generated.h"
#include "util/crypto.hh"

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
  std::vector<ToolSpec> specs;
  for (const auto& tool : catalog.tools())
    if (!guide || (!tool.mutating && tool.spec.name != "run_cancel" &&
                   tool.spec.name != "view_save"))
      specs.push_back(tool.spec);
  specs.push_back(
      {"document_template",
       "Get the initial definition and all source files for a new Card "
       "from the host's ui/templates catalog. name is the template id and id "
       "is the new Card "
       "identifier. Save definition_json and the returned files under "
       "definitions/<id>/, "
       "together with the canvas instance, in one project_save. This tool "
       "writes nothing.",
       R"({"type":"object","properties":{"name":{"type":"string"},"id":{"type":"string"}},"required":["name","id"],"additionalProperties":false})"});
  specs.push_back(
      {"document_connect",
       "Prepare a scope's connection edit, replacing an input wire and "
       "inserting or extending a fork when needed. state_json contains "
       "project, layout and "
       "definitions. Draft wire ends use an empty port (UI '+' gestures attach "
       "to the Card); "
       "they accept any number of wires without adding ports or forks. "
       "If needs_fork is true, obtain a new Card id and repeat with fork_id. "
       "Otherwise save "
       "the returned state_json together with project_save. This tool writes "
       "nothing.",
       R"({"type":"object","properties":{"state_json":{"type":"string"},"from_card":{"type":"string"},"from_port":{"type":"string"},"to_card":{"type":"string"},"to_port":{"type":"string"},"fork_id":{"type":"string"}},"required":["state_json","from_card","from_port","to_card","to_port"],"additionalProperties":false})"});
  specs.push_back(
      {"document_copy",
       "Prepare a definition or external copy from the current document state. "
       "from and to are project-relative folders ending in /. paths lists "
       "current source files. "
       "Returns edits_json (definition folders and external documents) and "
       "file mappings. "
       "Read and keep every mapped source byte before applying any change, "
       "then save the "
       "documents and bytes together with project_save. This tool writes "
       "nothing.",
       R"({"type":"object","properties":{"state_json":{"type":"string"},"paths":{"type":"array","items":{"type":"string"}},"from":{"type":"string"},"to":{"type":"string"},"name":{"type":"string"}},"required":["state_json","paths","from","to"],"additionalProperties":false})"});
  specs.push_back(
      {"document_merge",
       "Prepare a three-way merge of unsaved document states. "
       "Returns merged_json and conflict paths; show conflicts to the person "
       "before saving their choice with project_save. This tool writes "
       "nothing.",
       R"({"type":"object","properties":{"base_json":{"type":"string"},"ours_json":{"type":"string"},"theirs_json":{"type":"string"}},"required":["base_json","ours_json","theirs_json"],"additionalProperties":false})"});
  specs.push_back(
      {"document_make_subgraph",
       "Prepare a Card in Card edit from the current affected tsugumi.json. "
       "Returns project_json, external_json and link_json; move the selected "
       "definition folders and their external folders unchanged under "
       "externals/<id>/, then save all changes together with project_save. "
       "This tool writes nothing.",
       R"({"type":"object","properties":{"project_json":{"type":"string"},"cards":{"type":"array","items":{"type":"string"}},"id":{"type":"string"}},"required":["project_json","cards","id"],"additionalProperties":false})"});
  specs.push_back(
      {"demo_list", "List bundled read-only demo references",
       R"({"type":"object","properties":{},"additionalProperties":false})"});
  specs.push_back(
      {"demo_read", "Read a bundled demo reference",
       R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"],"additionalProperties":false})"});
  specs.push_back(
      {"guide_show", "Show guidance in connected user interfaces",
       R"({"type":"object","properties":{"say":{"type":"string"},"highlight":{"type":"string"},"open":{"type":"string"},"actions":{"type":"array"},"choices":{"type":"array","items":{"type":"string"}}}})"});
  if (!guide) {
    const auto definition =
        json::Object(json::Get(catalog.shared(), "delegateTool", "{}"));
    specs.push_back({"delegate", json::Text(definition, "description"),
                     json::Get(definition, "inputSchema", "{}")});
  }
  return specs;
}

// Read the exact revision being saved; never clean up a newer client's layout.
auto Tools::PrepareLayoutSave(const std::string& project,
                              const std::string& args, std::stop_token stop)
    -> std::string {
  struct StaleRevision {};
  std::optional<json::Members> snapshot;
  const auto fields = json::Object(args);
  const auto expected =
      json::Get(fields, fields.contains("base_rev") ? "base_rev" : "at_rev");
  auto read = [&](const std::string& path) -> std::optional<host::LayoutFile> {
    if (!snapshot) {
      const auto response =
          json::Object(ProjectCall(project, "project_inspect", "{}", stop));
      snapshot = json::Object(json::Get(response, "value", "{}"));
      const auto revision = json::Get(*snapshot, "rev");
      if (json::String(expected, expected) != json::String(revision, revision))
        throw StaleRevision{};
    }
    if (path == "tsugumi.json")
      return host::LayoutFile{json::Text(*snapshot, "project_json"),
                              std::nullopt};
    if (path == "layout.json")
      return host::LayoutFile{json::Text(*snapshot, "layout_json", "{}"),
                              std::nullopt};
    for (const auto& raw :
         json::Array(json::Get(*snapshot, "definitions", "[]"))) {
      const auto definition = json::Object(raw);
      if (path ==
          "definitions/" + json::Text(definition, "id") + "/_card.json") {
        const auto text = json::Text(definition, "card_json");
        return text.empty() ? std::nullopt
                            : std::optional<host::LayoutFile>{{text, {}}};
      }
    }
    std::string hash;
    for (const auto& raw : json::Array(json::Get(*snapshot, "sources", "[]"))) {
      const auto source = json::Object(raw);
      if (json::Text(source, "path") == path) hash = json::Text(source, "hash");
    }
    if (hash.empty()) return std::nullopt;
    std::string text;
    uint64_t offset = 0;
    for (;;) {
      const auto response = json::Object(ProjectCall(
          project, "project_file_read",
          json::Encode(json::Members{{"path", json::Quote(path)},
                                     {"at_rev", expected},
                                     {"if_hash", json::Quote(hash)},
                                     {"offset", std::to_string(offset)},
                                     {"limit_bytes", "1048576"}}),
          stop));
      if (json::Text(response, "reply") == "Failure")
        throw Failure("conflict", "Layout source changed while preparing save");
      const auto chunk = json::Object(json::Get(response, "value", "{}"));
      const auto bytes = json::Get(chunk, "bytes");
      if (!bytes.empty() && bytes.front() == '"')
        text += json::String(bytes);
      else {
        std::string decoded;
        if (!crypto::base64_decode(json::Text(json::Object(bytes), "base64"),
                                   decoded))
          throw Failure("invalid_request", "Invalid layout source bytes");
        text += decoded;
      }
      if (text.size() > 8 * 1024 * 1024)
        throw Failure("resource_limit", "Layout document exceeds 8 MiB");
      if (json::Bool(json::Get(chunk, "eof"))) break;
      const auto next = json::Get(chunk, "next_offset");
      const auto position = json::UInt(json::String(next, next));
      if (position <= offset)
        throw Failure("invalid_request", "Invalid layout source page");
      offset = position;
    }
    return host::LayoutFile{std::move(text), hash};
  };
  try {
    return host::PrepareLayoutSave(args, read);
  } catch (const StaleRevision&) {
    return args;
  }  // Let the core issue its normal CAS receipt.
}

auto Tools::SavedBase(const std::string& project, const std::string& rev)
    -> host::SaveBase {
  std::lock_guard lock(mutex);
  auto q = db.prepare(
      "SELECT snapshot,files FROM save_bases WHERE project=? AND rev=?");
  q.bind(1, project).bind(2, json::String(rev, rev));
  if (!q.step())
    throw SaveRevisionConflict("Unknown base_rev; inspect and re-apply");
  return {json::Object(q.text(0)), json::Object(q.text(1))};
}
auto Tools::RememberBase(const std::string& project,
                         const json::Members& snapshot, std::stop_token stop)
    -> host::SaveBase {
  const auto base = host::CaptureSaveBase(snapshot, [&](const auto& method,
                                                        const auto& query) {
    const auto reply = json::Object(
        RawProjectCall(project, method, json::Encode(query), stop));
    const auto value = json::Object(json::Get(reply, "value", "{}"));
    if (json::Text(reply, "reply") == "Failure")
      throw Failure(json::Text(value, "kind"), json::Text(value, "diagnostic"));
    return value;
  });
  StoreBase(project, base);
  return base;
}

auto Tools::StoreBase(const std::string& project, const host::SaveBase& base)
    -> void {
  std::lock_guard lock(mutex);
  auto q = db.prepare("INSERT OR REPLACE INTO save_bases VALUES(?,?,?,?)");
  q.bind(1, project)
      .bind(2, json::Text(base.snapshot, "rev"))
      .bind(3, json::Encode(base.snapshot))
      .bind(4, json::Encode(base.files))
      .run();
}

auto Tools::SaveProject(const std::string& project, json::Members original,
                        std::stop_token stop,
                        const std::function<void(const host::SaveBase&)>& check) -> std::string {
  std::lock_guard save_lock(save_mutex);
  const auto operation = json::Text(original, "op_id");
  auto failure = [](const std::string& kind, const std::string& message) {
    return json::Encode(json::Members{
        {"reply", json::Quote("Failure")},
        {"value",
         json::Encode(json::Members{{"kind", json::Quote(kind)},
                                    {"message", json::Quote(message)}})}});
  };
  auto read = [&](const auto& method, const auto& query) {
    const auto reply = json::Object(
        RawProjectCall(project, method, json::Encode(query), stop));
    const auto value = json::Object(json::Get(reply, "value", "{}"));
    if (json::Text(reply, "reply") == "Failure")
      throw Failure(json::Text(value, "kind"), json::Text(value, "diagnostic"));
    return value;
  };
  std::optional<AgentSave> prepared;
  {
    std::lock_guard lock(mutex);
    auto q = db.prepare(
        "SELECT arguments,comments FROM save_preparations WHERE operation=?");
    q.bind(1, operation);
    if (q.step()) prepared = AgentSave{json::Object(q.text(0)), q.text(1)};
  }
  auto attempt_id = operation;
  for (int attempt = 0; attempt < 8; ++attempt) {
    if (stop.stop_requested()) throw Cancelled();
    try {
      const auto snapshot = read("project_inspect", json::Members{});
      const auto latest = RememberBase(project, snapshot, stop);
      if (check) check(latest);
      if (!prepared) {
        auto args = original;
        if (json::String(json::Get(args, "base_rev"),
                         json::Get(args, "base_rev")) !=
            json::Text(snapshot, "rev")) {
          const auto base = SavedBase(project, json::Get(args, "base_rev"));
          try {
            args = host::RebaseSave(args, base, latest);
          } catch (const std::exception& e) {
            return failure("revision_conflict", e.what());
          }
        }
        args["op_id"] = json::Quote(attempt_id);
        prepared = PrepareAgentSave(args, read);
        prepared->arguments = json::Object(PrepareLayoutSave(
            project, json::Encode(prepared->arguments), stop));
        std::lock_guard lock(mutex);
        auto q = db.prepare(
            "INSERT OR REPLACE INTO save_preparations VALUES(?,?,?)");
        q.bind(1, operation)
            .bind(2, json::Encode(prepared->arguments))
            .bind(3, prepared->comments)
            .run();
      }
      if (original.contains("approved_permissions"))
        prepared->arguments["approved_permissions"] =
            original.at("approved_permissions");
      host::RestrictAgentRequest("project_save", prepared->arguments);
      const auto result = RawProjectCall(
          project, "project_save", json::Encode(prepared->arguments), stop);
      const auto envelope = json::Object(result);
      const auto value = json::Object(json::Get(envelope, "value", "{}"));
      if (json::Text(envelope, "reply") == "SaveResult") {
        const auto base = host::AdvanceSaveBase(
            SavedBase(project, json::Get(prepared->arguments, "base_rev")),
            value, prepared->arguments);
        StoreBase(project, base);
      }
      if (json::Text(envelope, "reply") != "Failure" ||
          json::Text(value, "kind") != "revision_conflict")
        return result;
      // A rejected CAS has its own durable receipt. A later attempt needs a
      // fresh id, kept under the original host operation for crash recovery.
      attempt_id = crypto::uuid4();
      prepared.reset();
    } catch (const Cancelled&) {
      throw;
    } catch (const SaveRevisionConflict& e) {
      if (std::string_view(e.what()) != "The document changed since base_rev")
        return failure("revision_conflict", e.what());
      prepared.reset();
    } catch (const Failure& e) {
      if (e.code != "revision_conflict") throw;
      prepared.reset();
    } catch (const host::LayoutFailure& e) {
      return LayoutToolFailure("invalid_document", e.what());
    } catch (const std::exception& e) {
      return failure("validation_failed", e.what());
    }
  }
  return failure("revision_conflict",
                 "Project kept changing during save; inspect and re-apply");
}

auto Tools::ProjectCall(const std::string& project, const std::string& name,
                        const std::string& arguments, std::stop_token stop)
    -> std::string {
  auto args = json::Object(arguments);
  if (name == "project_save")
    return SaveProject(project, std::move(args), stop);
  if (name == "operation_get") {
    std::lock_guard lock(mutex);
    auto q =
        db.prepare("SELECT arguments FROM save_preparations WHERE operation=?");
    q.bind(1, json::Text(args, "op_id"));
    if (q.step()) args["op_id"] = json::Get(json::Object(q.text(0)), "op_id");
  }
  if (name == "project_file_format") {
    H::FileFormatRequestT request;
    request.path = json::Text(args, "path");
    request.text = json::Text(args, "text");
    const auto language = json::Text(args, "language");
    bool known = language.empty();
    for (auto value : Tsugumi::Data::EnumValuesFormatLanguage())
      if (language == Tsugumi::Data::EnumNameFormatLanguage(value)) {
        request.language = value;
        known = true;
      }
    if (!known) throw Failure("unsupported", "Unknown formatting language");
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
  if (name == "version_graph") args.erase("language");
  if (name == "card_id") {
    args["stem"] = json::Quote(host::CardStem(json::Text(args, "name")));
    args.erase("name");
  }
  host::RestrictAgentRequest(name, args);
  if (name == "project_inspect") {
    const auto inspection = host::InspectForSave(
        [&](const auto& method, const auto& query) {
          const auto reply = json::Object(RawProjectCall(
              project, method, json::Encode(query), stop));
          const auto value = json::Object(json::Get(reply, "value", "{}"));
          if (json::Text(reply, "reply") == "Failure")
            throw Failure(json::Text(value, "kind"),
                          json::Text(value, "diagnostic"));
          return value;
        });
    StoreBase(project, inspection.base);
    return json::Encode(json::Members{
        {"reply", json::Quote("ProjectSnapshot")},
        {"value", json::Encode(inspection.snapshot)}});
  }
  auto result = RawProjectCall(project, name, json::Encode(args), stop);
  if (name == "version_graph") {
    auto envelope = json::Object(result);
    if (json::Text(envelope, "reply") == "VersionGraph") {
      std::string root;
      {
        std::lock_guard lock(mutex);
        auto path = db.prepare("SELECT path FROM project_paths WHERE id=?");
        path.bind(1, project);
        if (path.step()) root = path.text(0);
      }
      auto graph = host::PresentGraph(
          json::Object(json::Get(envelope, "value", "{}")), english);
      if (!root.empty()) {
        auto commits = json::Array(json::Get(graph, "commits", "[]"));
        for (auto& raw : commits) {
          auto commit = json::Object(raw);
          commit["comments"] = host::comments::record_json(
              host::comments::version(root, json::Text(commit, "id")));
          raw = json::Encode(commit);
        }
        graph["commits"] = json::Encode(commits);
      }
      envelope["value"] = json::Encode(graph);
      result = json::Encode(envelope);
    }
  }
  return result;
}

auto Tools::AnnotateSave(const std::string& operation,
                         const std::string& result) -> std::string {
  auto envelope = json::Object(result);
  if (json::Text(envelope, "reply") != "SaveResult") return result;
  std::lock_guard lock(mutex);
  auto saved =
      db.prepare("SELECT comments FROM save_preparations WHERE operation=?");
  saved.bind(1, operation);
  if (!saved.step()) return result;
  auto value = json::Object(json::Get(envelope, "value", "{}"));
  value["comments"] = saved.text(0);
  envelope["value"] = json::Encode(value);
  return json::Encode(envelope);
}

auto Tools::RawProjectCall(const std::string &project, const std::string &name,
                           const std::string &arguments, std::stop_token stop,
                           std::string *raw) -> std::string {
  auto prepared = arguments;
  uint64_t wait_seconds = 0;
  if (name == "run" || name == "run_status") {
    auto fields = json::Object(prepared);
    if (fields.contains("wait_seconds")) {
      wait_seconds = json::UInt(fields.at("wait_seconds"), UINT64_MAX);
      if (wait_seconds == UINT64_MAX)
        throw Failure("invalid_request",
                      "wait_seconds must be a nonnegative integer");
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
      const auto *note = R::GetMessage(bytes.data())->payload_as_Notification();
      const auto *event = note ? note->value_as_RunEvent() : nullptr;
      if (!event)
        return;
      const R::RunStatus *status = nullptr;
      if (const auto *changed = event->value_as_RunStatusChanged())
        status = changed->status();
      else if (const auto *finished = event->value_as_RunFinished())
        status = finished->summary()->status();
      if (!status || !status->ended_at())
        return;
      std::lock_guard lock(waiting->mutex);
      waiting->ended[event->run_id()->str()].reset(status->UnPack());
      waiting->changed.notify_all();
    });
  }
  std::stop_callback cancelled(stop, [&connection] { connection.Close(); });
  if (stop.stop_requested())
    throw Cancelled();
  // Cancelling closes the connection, also while it is still being set up:
  // that is a cancelled call, not an unreachable project.
  if (auto error = connection.Start().get()) {
    if (stop.stop_requested())
      throw Cancelled();
    throw Failure("unavailable", error.message);
  }
  auto hello =
      connection
          .Call([&](uint64_t id) {
            flatbuffers::FlatBufferBuilder b;
            auto h = R::CreateSessionHelloRequest(
                b, 1, b.CreateString(crypto::uuid4()), b.CreateString(project),
                endpoint.token.empty() ? 0 : b.CreateString(endpoint.token), 1);
            auto r = R::CreateRequest(b, R::RequestPayload::SessionHelloRequest,
                                      h.Union());
            R::FinishMessageBuffer(
                b, R::CreateMessage(b, id, R::Payload::Request, r.Union()));
            return client::Bytes(b.GetBufferPointer(),
                                 b.GetBufferPointer() + b.GetSize());
          })
          .get();
  if (stop.stop_requested())
    throw Cancelled();
  if (hello.error)
    throw Failure("unavailable", hello.error.message);
  const auto *reply = R::GetMessage(hello.bytes.data())->payload_as_Reply();
  if (!reply || reply->value_type() != R::ReplyPayload::HelloResult)
    throw Failure("unauthenticated", "Project rejected the Agent host");
  auto query = [&](const std::string &tool, const std::string &args) {
    auto response = connection
                        .Call([&](uint64_t id) {
                          auto bytes = EncodeProjectRequest(tool, args, id);
                          return client::Bytes(bytes.begin(), bytes.end());
                        })
                        .get();
    if (stop.stop_requested())
      throw Cancelled();
    if (response.error)
      throw Failure("unavailable", "Project connection lost while waiting: " +
                                       response.error.message);
    return response;
  };
  if (wait_seconds) {
    const auto opened = query("project_open", "{}");
    const auto *response =
        R::GetMessage(opened.bytes.data())->payload_as_Reply();
    if (!response || !response->value_as_OpenResult())
      throw Failure("unavailable",
                    "Could not subscribe to project run changes");
  }
  if (name == "project_save" || (name == "project_validate" &&
                                 json::Object(arguments).contains("changes"))) {
    try {
      prepared = PrepareLayoutSave(project, arguments, stop);
    } catch (const host::LayoutFailure &error) {
      // A host schema rejection is a completed tool result, so retrying the
      // same call does not look for a core mutation that was never sent.
      return LayoutToolFailure("invalid_document", error.what());
    }
  }
  auto result = connection
                    .Call([&](uint64_t id) {
                      auto bytes = EncodeProjectRequest(name, prepared, id);
                      return client::Bytes(bytes.begin(), bytes.end());
                    })
                    .get();
  if (stop.stop_requested())
    throw Cancelled();
  if (result.error)
    throw Failure("outcome_unknown", result.error.message);
  if (raw)
    raw->assign(reinterpret_cast<const char *>(result.bytes.data()),
                result.bytes.size());
  auto output =
      catalog.Reply({reinterpret_cast<const char *>(result.bytes.data()),
                     result.bytes.size()});
  if (!wait_seconds)
    return output;
  auto envelope = json::Object(output);
  if (json::Text(envelope, "reply") == "Failure")
    return output;
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
    if (stop.stop_requested())
      throw Cancelled();
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
                        {reinterpret_cast<const char *>(b.GetBufferPointer()),
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
          catalog.Reply({reinterpret_cast<const char *>(current.bytes.data()),
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

auto Tools::HelperReader(const std::string& conversation, std::stop_token stop,
                         const std::string& revision,
                         const std::string& authorization)
    -> HelperScope::Reader {
  std::string project;
  {
    std::lock_guard lock(mutex);
    project = conversations->GetConversation(conversation).project;
  }
  auto cache = std::make_shared<std::map<std::string, std::string>>();
  return [this, project, conversation, stop, revision, authorization,
          cache](const std::string& path) {
    access_service.Require(project, conversation,
                           Tsugumi::Service::Access::Read, true, stop,
                           "Read helper Card context", authorization);
    if (const auto found = cache->find(path); found != cache->end())
      return found->second;
    if (!revision.empty()) {
      try {
        const auto base = SavedBase(project, revision);
        if (base.files.contains(path)) return base.files.at(path);
        if (path.ends_with("/_link.json") || path.ends_with("/_card.json") ||
            path.ends_with("/_meta.json"))
          return std::string{};
      } catch (const SaveRevisionConflict&) {
        // A read without a prior inspection still uses the core's CAS check.
      }
    }
    json::Members args{{"path", json::Quote(path)}, {"limit_bytes", "1048576"}};
    if (!revision.empty()) args["at_rev"] = revision;
    std::string bytes;
    for (;;) {
      args["offset"] = std::to_string(bytes.size());
      const auto reply = json::Object(
          ProjectCall(project, "project_file_read", json::Encode(args), stop));
      if (json::Text(reply, "reply") != "FileContent") {
        const auto failure = json::Object(json::Get(reply, "value", "{}"));
        if (json::Text(reply, "reply") == "Failure" &&
            json::Text(failure, "kind") == "not_found") {
          cache->emplace(path, "");
          return std::string{};
        }
        throw Failure("permission_denied",
                      "Helper scope cannot resolve Card file " + path);
      }
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
    cache->emplace(path, bytes);
    return bytes;
  };
}

auto Tools::ScopedResult(HelperScope& scope, const ToolCall& call,
                         ToolResult result, const HelperScope::Reader& read)
    -> ToolResult {
  if (!scope.scoped() || call.name == "tool_result_read") return result;
  if (result.is_error) {
    if (catalog.Find(call.name)) {
      const auto envelope = json::Object(result.text);
      const auto failure = json::Object(json::Get(envelope, "value", "{}"));
      json::Members safe;
      for (const auto* key : {"kind", "current_rev", "retry_after_ms"})
        if (failure.contains(key)) safe[key] = failure.at(key);
      const auto message = json::Text(failure, "message");
      if (json::Text(failure, "kind") == "revision_conflict" &&
          scope.SafeCollision(message, read))
        safe["message"] = json::Quote(message);
      safe["diagnostic"] = json::Quote(
          "Card-scoped operation failed. Reinspect on revision_conflict; "
          "otherwise ask the parent to inspect the full failure in this "
          "tool's saved receipt.");
      result.text = json::Encode(json::Members{
          {"reply", json::Quote("Failure")}, {"value", json::Encode(safe)}});
    }
    return result;
  }
  scope.Completed(call.name, json::Object(call.arguments), result.text, read);
  if (call.name == "project_inspect")
    result.text = scope.Inspect(result.text, read);
  else if (call.name == "project_save")
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

auto Tools::RequireHelperPermission(const std::string &conversation,
                                    const HelperScope &boundary) -> void {
  if (!boundary.scoped() &&
      !host_settings->AllowUnscopedHelpers(providers->Profile(conversation)))
    throw Failure("permission_denied",
                  "Unscoped helpers are disabled; ask the parent for "
                  "explicit read_cards, write_cards and run_cards");
}

auto Tools::ExecuteTool(const std::string& conversation,
                        const std::string& requested_project,
                        const ToolCall& call, bool wait, std::stop_token stop,
                        bool helper, HelperScope* boundary) -> ToolResult {
  Conversation c;
  {
    std::lock_guard lock(mutex);
    c = conversations->GetConversation(conversation);
  }
  const auto project =
      requested_project.empty() ? c.project : requested_project;
  auto args = json::Object(call.arguments);
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
        const auto scope =
            crypto::sha256_hex(q.text(0) + ":" + std::to_string(q.i64(1))) +
            ":";
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
        access_service.AuthorizeContext(conversation, {message});
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
        auto end = std::min(block.text.size(), begin + static_cast<size_t>(limit));
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
      call.name == "log_range" || call.name == "module_read") {
    if (!args.contains("limit_bytes"))
      args["limit_bytes"] = std::to_string(kResultPageBytes);
  }
  if (call.name == "run_history" || call.name == "run_invocations" ||
      call.name == "artifact_list" || call.name == "run_environments" ||
      call.name == "run_environment_paths") {
    if (!args.contains("limit")) args["limit"] = "8";
  }
  if (call.name == "document_template") {
    const auto edit =
        host::WriteTemplate(json::Text(args, "name"), json::Text(args, "id"));
    std::vector<std::string> files;
    for (const auto& file : edit.files)
      files.push_back(
          json::Encode(json::Members{{"path", json::Quote(file->path)},
                                     {"text", json::Quote(file->text)}}));
    return {json::Encode(json::Members{
                {"definition_json", json::Quote(edit.definition_json)},
                {"files", json::Encode(files)}}),
            false,
            {},
            {}};
  }
  if (call.name == "document_connect") {
    const auto edit = host::ConnectDocuments(
        json::Text(args, "state_json"), json::Text(args, "from_card"),
        json::Text(args, "from_port"), json::Text(args, "to_card"),
        json::Text(args, "to_port"), json::Text(args, "fork_id"));
    return {json::Encode(json::Members{
                {"state_json", json::Quote(edit.state_json)},
                {"inserted_id", json::Quote(edit.inserted_id)},
                {"needs_fork", edit.needs_fork ? "true" : "false"}}),
            false,
            {},
            {}};
  }
  if (call.name == "document_copy") {
    std::vector<std::string> paths;
    for (const auto& path : json::Array(json::Get(args, "paths", "[]")))
      paths.push_back(json::String(path));
    const auto edit = host::CopyDocuments(
        json::Text(args, "state_json"), paths, json::Text(args, "from"),
        json::Text(args, "to"), json::Text(args, "name"));
    std::vector<std::string> files;
    for (const auto& file : edit.files)
      files.push_back(json::Encode(json::Members{
          {"from", json::Quote(file->from)}, {"to", json::Quote(file->to)}}));
    return {
        json::Encode(json::Members{{"edits_json", json::Quote(edit.edits_json)},
                                   {"files", json::Encode(files)}}),
        false,
        {},
        {}};
  }
  if (call.name == "document_merge") {
    const auto edit = host::MergeDocuments(json::Text(args, "base_json"),
                                           json::Text(args, "ours_json"),
                                           json::Text(args, "theirs_json"));
    std::vector<std::string> conflicts;
    for (const auto& path : edit.conflicts)
      conflicts.push_back(json::Quote(path));
    return {json::Encode(
                json::Members{{"merged_json", json::Quote(edit.merged_json)},
                              {"conflicts", json::Encode(conflicts)}}),
            false,
            {},
            {}};
  }
  if (call.name == "document_make_subgraph") {
    std::vector<std::string> cards;
    for (const auto& card : json::Array(json::Get(args, "cards", "[]")))
      cards.push_back(json::String(card));
    const auto edit = host::MakeSubgraph(json::Text(args, "project_json"),
                                         cards, json::Text(args, "id"));
    return {json::Encode(json::Members{
                {"project_json", json::Quote(edit.project_json)},
                {"external_json", json::Quote(edit.external_json)},
                {"link_json", json::Quote(edit.link_json)}}),
            false,
            {},
            {}};
  }
  if (call.name == "demo_list") return {demo_references.List(), false, {}, {}};
  if (call.name == "demo_read")
    return {demo_references.Read(json::Text(args, "path")), false, {}, {}};
  if (call.name == "guide_show") {
    dependencies.event(
        conversation,
        json::Encode(json::Members{{"kind", json::Quote("toolStarted")},
                                   {"id", json::Quote(call.id)},
                                   {"name", json::Quote(call.name)},
                                   {"input", json::Quote(call.arguments)}}));
    return {"Guidance sent to connected interfaces", false, {}, {}};
  }
  if (call.name == "delegate") {
    if (c.role == "guide")
      return {"Guide cannot delegate", true, {}, {}};
    if (boundary)
      RequireHelperPermission(conversation, *boundary);
    auto tasks = json::Array(json::Get(args, "tasks", "[]"));
    if (tasks.empty()) return {"Provide at least one task", true, {}, {}};
    const auto policy =
        host_settings->AllowUnscopedHelpers(providers->Profile(conversation));
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
               boundary ? boundary->authorization() : std::string{}] {
            return dependencies.run_helper(conversation, id, i, task, stop,
                                           authorization);
          }));
    }
    std::vector<std::string> results;
    for (auto& future : pending) results.push_back(future.get());
    std::lock_guard lock(mutex);
    return {json::Encode(results),
            false,
            {},
            conversations->Resources(conversation)};
  }
  const auto* spec = catalog.Find(call.name);
  if (!spec) throw Failure("not_found", "Unknown tool");
  if (project.empty())
    throw Failure("invalid_request",
                  "Select a project before using project tools");
  const auto authorization =
      boundary ? boundary->authorization() : std::string{};
  access_service.Require(project, conversation, Tsugumi::Service::Access::Read,
                         wait, stop, "Read project content", authorization);
  if (boundary) {
    RequireHelperPermission(conversation, *boundary);
    std::string revision = json::Get(args, "base_rev", "");
    if (boundary->scoped() &&
        (call.name == "project_file_list" || call.name == "project_file_read" ||
         call.name == "project_file_stat" || call.name == "project_validate" ||
         call.name == "run_plan" || call.name == "card_s_d_k_prepare")) {
      revision = json::Get(args, "at_rev", "");
      if (revision.empty()) {
        const auto snapshot =
            json::Object(ProjectCall(project, "project_inspect", "{}", stop));
        revision =
            json::Get(json::Object(json::Get(snapshot, "value", "{}")), "rev");
      }
      args["at_rev"] = revision;
    }
    // Resolve nested file ownership and read the file at one revision. If
    // the parent changes a container link in between, the project rejects
    // the stale at_rev/base_rev instead of reading a different Card.
    const auto read = HelperReader(conversation, stop, revision, authorization);
    boundary->Check(call.name, args, read);
    if (boundary->scoped() && (call.name == "run" || call.name == "run_plan" ||
                               call.name == "card_s_d_k_prepare")) {
      json::Members plan{{"card", json::Get(args, "card")}};
      if (call.name == "card_s_d_k_prepare")
        plan["propagate"] = "false";
      else if (args.contains("propagate"))
        plan["propagate"] = args.at("propagate");
      if (args.contains("base_rev"))
        plan["at_rev"] = args.at("base_rev");
      else if (args.contains("at_rev"))
        plan["at_rev"] = args.at("at_rev");
      const auto result =
          ProjectCall(project, "run_plan", json::Encode(plan), stop);
      boundary->CheckPlan(result, call.name != "run_plan");
      // A run must supply the same base_rev used by this plan. The project
      // rejects a missing or stale revision before executing anything.
    }
  }
  const bool execution = call.name == "run" || call.name == "run_cancel" ||
                         call.name == "card_s_d_k_prepare" ||
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
    const bool english = host_settings->Language(providers->Profile(conversation)) == "en";
    const std::string reason = english
        ? "Execute shell commands as the project user in its environment. "
          "Commands can change writable Card source files through a validated save. "
          "Attached folders and structural documents are read-only. Command: "
        : "プロジェクトの実行環境でシェルコマンドを実行します。"
          "書き込みが許可された Card のソースを編集し、検証して保存できます。"
          "添付フォルダと構造文書は読み取り専用です。コマンド: ";
    access_service.Require(
        project, conversation, Tsugumi::Service::Access::Shell, wait, stop,
        reason + json::Text(args, "command"), {});
  }
  std::string folder_resource;
  if (call.name == "attached_folder_read" ||
      (call.name == "project_copy" && json::Text(args, "source") == "attached")) {
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
                           "Read the user-selected attached folder", {});
  }
  for (const auto* field :
       {"origin", "approved_permissions", "check_permissions", "read_only",
        "hide_credentials", "no_project_socket", "transfers", "working_directory",
        "writable_paths", "mounts", "protected_paths", "workspace_only", "output_prefix"})
    args.erase(field);
  if (call.name == "run_history_delete") {
    args.erase("run_ids");
    args.erase("restrict_to_run_ids");
  }
  if (spec->mutating) args.erase("op_id");
  const auto digest = crypto::sha256_hex(call.name + "\n" + project + "\n" +
                                         json::Encode(args));
  const auto op = crypto::sha256_hex(conversation + ":" + call.id);
  auto op_id = op.substr(0, 8) + "-" + op.substr(8, 4) + "-4" +
               op.substr(13, 3) + "-8" + op.substr(17, 3) + "-" +
               op.substr(20, 12);
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
                          conversations->Resources(conversation));
      op_id = old.text(4);
      interrupted = true;
      if (!spec->approval_required) {
        auto approval = db.prepare(
            "SELECT a.state,o.request,o.preview FROM operation_approvals o "
            "JOIN access_requests a ON a.id=o.request "
            "WHERE o.conversation=? AND o.call=?");
        approval.bind(1, conversation).bind(2, call.id);
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
    const auto run_ids = dependencies.approve_operation(
        project, conversation, call, digest, args, wait, stop, {});
    if (call.name == "run_history_delete") {
      args["run_ids"] = json::Encode(run_ids);
      args["restrict_to_run_ids"] = "true";
    }
  }
  if (spec->approval_required && !interrupted) {
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
  // binding checks are identical on the initial send, resend and CAS retries.
  auto project_call = [&]() {
    if (!boundary || !boundary->scoped() || call.name != "project_save")
      return ProjectCall(project,
                         (call.name == "project_copy" || call.name == "project_shell")
                             ? "project_save" : call.name,
                         json::Encode(args), stop);
    const auto before = HelperReader(
        conversation, stop, json::Get(args, "base_rev"), authorization);
    const auto expanded = boundary->ExpandSave(args, before);
    return SaveProject(
        project, expanded, stop, [&](const host::SaveBase& latest) {
          boundary->CheckBindings(before, [&](const std::string& path) {
            const auto found = latest.files.find(path);
            return found == latest.files.end() ? std::string{} : found->second;
          });
        });
  };
  // An interrupted mutation can only be resolved by its durable receipt.
  // Reissuing it after a daemon restart may execute it a second time.
  std::string result;
  std::vector<std::string> file_resources;
  if (interrupted && spec->mutating) {
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
      const auto waited = json::Object(ProjectCall(
          project, "run_status", json::Encode(json::Members{
              {"run_id", json::Get(status, "run_id")},
              {"wait_seconds", json::Get(args, "wait_seconds")}}), stop));
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
          host_settings->Language(providers->Profile(conversation)));
    if (call.name == "project_copy" || call.name == "project_shell") {
      result = FileOperation(conversation, project, call.name, args, op_id, boundary, stop, file_resources);
    } else if (call.name == "attached_folder_read") {
      try {
        result = host::ReadAttachedFolder(json::Text(args, "folder"), args);
      } catch (const host::FolderReadFailure& error) {
        throw Failure(error.code, error.what());
      }
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
      value["hint"] = json::Quote("Use project_copy to move files without reading them, or project_shell to examine large files. Reads default to 256 KiB; maximum 1 MiB.");
      page["value"] = json::Encode(value);
      result = json::Encode(page);
    }
  }
  const auto envelope = json::Object(result);
  bool error = json::Text(envelope, "reply") == "Failure";
  if (error) {
    auto failure = json::Object(json::Get(envelope, "value", "{}"));
    if (json::Text(failure, "kind") == "approval_required") {
      if (!authorization.empty())
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
      dependencies.approve_operation(project, conversation, call, digest, args,
                                     wait, stop, permissions);
      args["approved_permissions"] = json::Encode(permissions);
      op_id = crypto::uuid4();
      args["op_id"] = json::Quote(op_id);
      {
        std::lock_guard lock(mutex);
        sql::Transaction tx(conversations->ConversationDatabase(conversation));
        auto receipt = conversations->ConversationDatabase(conversation)
                           .prepare(
                               "UPDATE receipts SET operation=? WHERE "
                               "conversation=? AND id=?");
        receipt.bind(1, op_id).bind(2, conversation).bind(3, call.id).run();
        tx.commit();
      }
      result = project_call();
      error = json::Text(json::Object(result), "reply") == "Failure";
    }
  }
  if (call.name == "project_save") result = AnnotateSave(op_id, result);
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
    if (json::Text(envelope, "reply") == "SaveResult") value = compact_save(value);
    if (value.contains("save")) value["save"] = json::Encode(compact_save(json::Object(value.at("save"))));
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
    if (!access_service.HasGrant(
            authorization.empty() ? project : authorization, conversation,
            Tsugumi::Service::Access::Read))
      throw Failure("permission_denied",
                    "Access was revoked while the operation was running");
    for (const auto& resource : file_resources)
      if (!access_service.HasGrant(resource, conversation, Tsugumi::Service::Access::Read))
        throw Failure("permission_denied", "Folder access was revoked during the operation");
    conversations->RememberResource(
        conversation, authorization.empty() ? project : authorization);
    if (!folder_resource.empty()) {
      if (!access_service.HasGrant(folder_resource, conversation,
                                   Tsugumi::Service::Access::Read))
        throw Failure("permission_denied",
                      "Folder access was revoked while reading");
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
  std::vector<std::string> resources{authorization.empty() ? project
                                                           : authorization};
  if (!folder_resource.empty()) resources.push_back(folder_resource);
  resources.insert(resources.end(), file_resources.begin(), file_resources.end());
  return ToolOutput(result, error, std::move(resources));
}

auto Tools::Hooks(const std::string& id, bool helper,
                  std::shared_ptr<HelperScope> boundary) -> RuntimeHooks {
  RuntimeHooks hooks;
  hooks.authorize_context = [this, id](const auto& messages) {
    access_service.AuthorizeContext(id, messages);
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
                                       {"input", json::Quote(e.text)},
                                       {"id", json::Quote(e.id)},
                                       {"name", json::Quote(e.name)},
                                       {"ok", e.ok ? "true" : "false"},
                                       {"index", std::to_string(e.index)}}));
  };
  hooks.reconcile_tool =
      [this, id, boundary](const ToolCall& call) -> std::optional<ToolResult> {
    std::lock_guard lock(mutex);
    if (call.name == "delegate") {
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
          results.push_back(dependencies.helper_result(
              helpers.text(0), id, call.id, helpers.i64(2)));
      }
      const auto tasks =
          json::Array(json::Get(json::Object(call.arguments), "tasks", "[]"));
      incomplete |= results.size() != tasks.size();
      if (!incomplete)
        return ToolResult{
            json::Encode(results), false, {}, conversations->Resources(id)};
      return ToolResult{
          "Helpers were interrupted. Their saved messages and operation "
          "receipts remain in the conversation; inspect them before "
          "assigning more work. Completed results: " +
              json::Encode(results),
          true,
          {},
          conversations->Resources(id)};
    }
    auto q = conversations->ConversationDatabase(id).prepare(
        "SELECT state,result,error,project,operation FROM "
        "receipts WHERE conversation=? AND id=?");
    q.bind(1, id).bind(2, call.id);
    if (!q.step()) return std::nullopt;
    if (!access_service.HasGrant(boundary && !boundary->authorization().empty()
                                     ? boundary->authorization()
                                     : q.text(3),
                                 id, Tsugumi::Service::Access::Read))
      return ToolResult{"Project access has been revoked", true, {}, {}};
    if (q.text(0) == "completed")
      return ToolOutput(
          q.text(1), q.i64(2) != 0,
          boundary && !boundary->authorization().empty()
              ? std::vector<std::string>{boundary->authorization()}
              : conversations->Resources(id));
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
        [this, id, boundary,
         reconcile](const ToolCall& call) -> std::optional<ToolResult> {
      try {
        RequireHelperPermission(id, *boundary);
        if (call.name == "delegate") {
          // Delegate has no core receipt and is not a project tool. Match its
          // lifetime reservation before returning saved children; do not charge
          // the capability again or launch work during reconciliation.
          const auto tasks = json::Array(
              json::Get(json::Object(call.arguments), "tasks", "[]"));
          for (const auto &task : tasks)
            boundary->CheckChild(json::Object(task));
          std::lock_guard lock(mutex);
          auto reserved = conversations->ConversationDatabase(id).prepare(
              "SELECT digest,count FROM helper_delegations WHERE "
              "conversation=? AND parent=? AND call=?");
          reserved.bind(1, id).bind(2, boundary->identity).bind(3, call.id);
          if (!reserved.step())
            return std::nullopt;
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
      const ToolCall call{p->op_id()->str(), p->name()->str(),
                          p->arguments_json()->str()};
      std::shared_ptr<std::stop_source> shell_stop;
      const auto shell_key = std::make_pair(id, call.id);
      if (call.name == "project_shell" || call.name == "project_copy" ||
          call.name == "run_status" || call.name == "run") {
        if (file_calls.contains(shell_key))
          throw Failure("busy", "This tool call is already running");
        shell_stop = std::make_shared<std::stop_source>();
        file_calls.emplace(
            shell_key,
            FileCall{project.empty()
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
          Tools &tools;
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
        if (shell_stop->stop_requested())
          throw Cancelled();
        auto event = [&](const char *kind, const std::string &text, bool ok) {
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
        } catch (const std::exception &error) {
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
