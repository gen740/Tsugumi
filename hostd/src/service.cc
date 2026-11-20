#include "host/service.hh"

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

#include "agent/approvals.hh"
#include "agent/helpers.hh"
#include "agent/providers.hh"
#include "agent/store.hh"
#include "agent/tools.hh"
#include "agent/turns.hh"
#include "agent/wire.hh"
#include "host/access.hh"
#include "host/binary_caches.hh"
#include "host/credentials.hh"
#include "host/debug_trace.hh"
#include "host/demo_references.hh"
#include "host/document_edits.hh"
#include "host/environment.hh"
#include "host/events.hh"
#include "host/execution_broker.hh"
#include "host/failure.hh"
#include "host/format.hh"
#include "host/guide.hh"
#include "host/identifiers.hh"
#include "host/layout_service.hh"
#include "host/models.hh"
#include "host/permission_kinds.hh"
#include "host/projects.hh"
#include "host/registry.hh"
#include "host/settings.hh"
#include "host/skills_service.hh"
#include "host/state_gate.hh"
#include "host/store.hh"
#include "host/templates.hh"
#include "host/versions.hh"
#include "host/wire_helpers.hh"
#include "tsugumi/rpc/lanes.hh"
#include "tsugumi/util/crypto.hh"

namespace tsugumi::host {
namespace A = Tsugumi::Agent;
namespace H = Tsugumi::Host;
namespace S = Tsugumi::Service;
namespace json = ::tsugumi::json;
namespace wire = agent::wire;
using agent::Approvals;
using agent::Cancelled;
using agent::ConversationStore;
using agent::Helpers;
using agent::Providers;
using agent::Tools;
using agent::Turns;
namespace {

using host::Failure;
auto Failed(uint64_t id, std::string_view code, std::string_view message,
            std::string_view request = {}) -> std::string {
  S::FailureT failure;
  failure.code = code;
  failure.message = message;
  failure.request_id = request;
  return wire::AgentReply(id, std::move(failure));
}
auto HostFailed(uint64_t id, std::string_view code, std::string_view message,
                std::string_view request = {}) -> std::string {
  S::FailureT failure;
  failure.code = code;
  failure.message = message;
  failure.request_id = request;
  return wire::HostReply(id, std::move(failure));
}

}  // namespace

struct Service::State {
  using Conversation = ConversationStore::Conversation;
  using Project = host::ProjectRegistry::Project;
  explicit State(HostOptions o)
      : options(std::move(o)),
        host_store(options.state_dir),
        db(host_store.db),
        credentials(state_gate, db, shutdown.get_token()),
        registry(state_gate, db,
                 [this](const std::string& project)
                     -> std::optional<std::filesystem::path> {
                   return conversations ? conversations->OpenedPath(project)
                                        : std::nullopt;
                 }),
        host_projects(options.state_dir, options.demos_dir, &db),
        demo_references(options.demos_dir),
        guide(options.state_dir, options.demos_dir),
        requests(4),
        trace_requests(1),
        tool_requests(4),
        wait_requests(4),
        terminal_requests(1),
        terminal_closes(1),
        environment_stops(2) {
    host_settings = std::make_unique<host::Settings>(
        options.state_dir,
        [this](const std::string& id, const json::Members& values) {
          credentials.Import(id, values);
        },
        [this](const std::string& id, std::string_view stale) {
          return credentials.AccessToken(id, stale);
        },
        [this](const std::string& id) { return credentials.Forget(id); },
        nullptr, host::Settings::kKeyStorage,
        [this] { return host::Models::StoredChatGPTModels(options.state_dir); },
        [this](const std::string& profile) {
          requests.submit([this, profile]() {
            std::lock_guard lock(mutex);
            H::ProviderChangedT event;
            event.profile = profile;
            events->Publish(wire::HostEvent(std::move(event)));
          });
        });
    providers = std::make_unique<Providers>(state_gate, db, *host_settings);
    conversations = std::make_unique<ConversationStore>(
        state_gate, db, registry,
        [this](std::string raw) { return Scrub(std::move(raw)); },
        [this](const std::string& resource, const std::string& id,
               Tsugumi::Service::Access access) {
          return this->access->HasGrant(resource, id, access);
        },
        [this](const std::string& id) { return turns->running.contains(id); });
    access = std::make_unique<host::Access>(
        state_gate, db, registry, *conversations, stopping,
        host::Access::Dependencies{
            .event = [this](const auto& id,
                            const auto& raw) { Event(id, raw); },
            .cancel =
                [this](const auto& project, const auto& conversation) {
                  for (const auto& [key, call] : tools->file_calls)
                    if (call.project == project &&
                        (conversation.empty() || key.first == conversation))
                      call.stop->request_stop();
                  // A model-issued shell uses its conversation's stop token.
                  for (const auto& [id, job] : turns->jobs)
                    if ((conversation.empty() || id == conversation) &&
                        conversations->GetConversation(id).project == project)
                      job->request_stop();
                }});
    turns = std::make_unique<Turns>(
        state_gate, db, *conversations, *providers, *host_settings, shutdown,
        stopping, access->changed, *access,
        Turns::Dependencies{
            .scrub = [this](std::string raw) { return Scrub(std::move(raw)); },
            .event = [this](const auto& id,
                            const auto& raw) { Event(id, raw); },
            .hooks = [this](const auto& id) { return tools->Hooks(id); },
            .specs = [this](bool guide) { return tools->Specs(guide); },
            .run_helper =
                [this](const auto& id, const auto& call, size_t index,
                       const auto& task, auto stop, const auto& authorization) {
                  return helpers->RunHelper(id, call, index, task, stop,
                                            authorization);
                },
            .cancel_file_tools =
                [this](const auto& id) {
                  for (const auto& [key, source] : tools->file_calls)
                    if (key.first == id) source.stop->request_stop();
                },
            .excluded =
                [this](const auto& project, const auto& path, auto stop) {
                  return !project.empty() &&
                         tools->Exclusions(project, stop).Contains(path);
                }});
    tools = std::make_unique<Tools>(
        state_gate, db, *conversations, *providers, *host_settings,
        demo_references, *turns, shutdown, *access,
        Tools::Dependencies{
            .scrub = [this](std::string raw) { return Scrub(std::move(raw)); },
            .event = [this](const auto& id,
                            const auto& raw) { Event(id, raw); },
            .approve_operation =
                [this](const auto& project, const auto& id, const auto& call,
                       const auto& digest, const auto& args, bool wait,
                       auto stop, const auto& permissions) {
                  return approvals->ApproveOperation(
                      project, id, call, digest, args, wait, stop, permissions);
                },
            .run_helper =
                [this](const auto& id, const auto& call, size_t index,
                       const auto& task, auto stop, const auto& authorization) {
                  return helpers->RunHelper(id, call, index, task, stop,
                                            authorization);
                },
            .helper_result =
                [this](const auto& output, const auto& id, const auto& call,
                       size_t index) {
                  return helpers->HelperResult(output, id, call, index);
                }});
    approvals =
        std::make_unique<Approvals>(state_gate, db, *conversations, *tools,
                                    stopping, access->changed, *access);
    helpers = std::make_unique<Helpers>(
        state_gate, *conversations, *providers, *host_settings, *tools, *turns,
        *access,
        Helpers::Dependencies{
            .scrub = [this](std::string raw) { return Scrub(std::move(raw)); },
            .event = [this](const auto& id,
                            const auto& raw) { Event(id, raw); },
        });
    skills_service = std::make_unique<host::SkillsService>(db, *host_settings);
    models = std::make_unique<host::Models>(
        db, *host_settings, shutdown.get_token(),
        [this](std::string raw) { return Scrub(std::move(raw)); });
    versions = std::make_unique<host::Versions>(
        db, registry, *host_settings, *tools, *turns,
        [this](std::string raw) { return Scrub(std::move(raw)); });
    events = std::make_unique<host::HostEvents>(
        state_gate, terminal_closes, [this](uint64_t session) {
          H::HostTerminalRequestT close;
          close.action = H::TerminalAction::Close;
          close.session = session;
          environments->Terminal(close);
        });
    if (!options.broker_socket.empty())
      execution_broker = std::make_unique<host::ExecutionBroker>(
          options.state_dir, options.broker_socket);
    if (!options.vm.binary.empty()) {
      options.vm.broker_socket = execution_broker->socket_path();
      options.vm.resources = [this] { return host_settings->VMResources(); };
      options.vm.progress = [this](const Tsugumi::VMHost::ProgressT& value) {
        H::EnvironmentProgressT progress;
        progress.project = "";
        progress.key = value.key;
        progress.stage = value.stage;
        progress.fraction = value.fraction;
        progress.preparing = value.preparing;
        std::lock_guard lock(mutex);
        events->Publish(wire::HostEvent(std::move(progress)));
      };
      options.vm.runtime = [this](const H::HostRuntimeT& value) {
        std::lock_guard lock(mutex);
        events->Publish(wire::HostEvent(value));
      };
      options.vm.terminal = [this](const H::HostTerminalDataT& value) {
        std::lock_guard lock(mutex);
        events->Publish(wire::HostEvent(value));
      };
      options.environment.process = host::VMEnvironmentProcess(options.vm);
    }
    if (!options.environment.process)
      options.environment.process = host::NativeEnvironmentProcess(
          [this](const H::HostTerminalDataT& value) {
            std::lock_guard lock(mutex);
            events->Publish(wire::HostEvent(value));
          });
    options.environment.reserve_project_identity =
        [this](const std::filesystem::path& root) {
          std::lock_guard lock(mutex);
          const auto file = root / ".tsugumi/project-id";
          if (const auto id = fs::read_file(file);
              id && crypto::is_short_id(*id)) {
            db.prepare("INSERT OR IGNORE INTO identifiers VALUES('project',?)")
                .bind(1, *id)
                .run();
            return registry.ReserveLocation(*id, root);
          }
          std::string id;
          for (;;) {
            id = host::AllocateId(db, "project");
            auto known = db.prepare("SELECT 1 FROM projects WHERE id=?");
            known.bind(1, id);
            if (!known.step()) break;
          }
          auto release = registry.ReserveLocation(id, root);
          try {
            fs::write_atomic(file, id, 0600);
          } catch (...) {
            release();
            throw;
          }
          return release;
        };
    if (execution_broker)
      environments = std::make_unique<host::Environments>(
          options.environment,
          host::EnvironmentAuthorization{
              [this](const std::string& key, const std::filesystem::path& path,
                     const std::filesystem::path& control) {
                return execution_broker->Authorize(
                    key, path, control,
                    options.environment.process &&
                        options.environment.process->Guest());
              },
              [this](const std::string& key) {
                execution_broker->Revoke(key);
              }},
          [this](const H::EnvironmentStateT& state) {
            EnvironmentChanged(state);
          },
          [this](const H::EnvironmentProgressT& progress) {
            std::lock_guard lock(mutex);
            events->Publish(wire::HostEvent(progress));
          });
    // Resume observing resident native daemons even before a UI reconnects.
    // This restores process capabilities, never cached run data.
    if (environments) {
      std::vector<std::pair<std::string, std::string>> resident;
      {
        std::lock_guard lock(mutex);
        auto q = db.prepare(
            "SELECT e.key,p.path FROM environment_projects e JOIN "
            "project_paths p ON p.id=e.project");
        while (q.step()) resident.emplace_back(q.text(0), q.text(1));
      }
      for (const auto& [key, path] : resident) {
        try {
          H::EnvironmentRequestT request;
          request.project = path;
          environments->Handle(request);
        } catch (const std::exception&) {
          execution_broker->Revoke(key);
          H::EnvironmentStateT stopped;
          stopped.key = key;
          stopped.project = path;
          EnvironmentChanged(stopped);
        }
      }
    }
  }

  HostOptions options;
  host::StateGate state_gate;
  host::HostStore host_store;
  sql::Db& db;
  std::stop_source shutdown;
  host::Credentials credentials;
  host::ProjectRegistry registry;
  std::unique_ptr<ConversationStore> conversations;
  host::Projects host_projects;
  host::DemoReferences demo_references;
  host::Guide guide;
  std::unique_ptr<host::Settings> host_settings;
  std::unique_ptr<Providers> providers;
  std::unique_ptr<Turns> turns;
  std::unique_ptr<Tools> tools;
  std::unique_ptr<Approvals> approvals;
  std::unique_ptr<Helpers> helpers;
  std::unique_ptr<host::Access> access;
  std::unique_ptr<host::SkillsService> skills_service;
  std::unique_ptr<host::Models> models;
  std::unique_ptr<host::Versions> versions;
  std::unique_ptr<host::HostEvents> events;
  std::unique_ptr<host::ExecutionBroker> execution_broker;
  std::unique_ptr<host::Environments> environments;
  TransportServer* server = nullptr;
  std::recursive_mutex& mutex = state_gate.mutex;
  bool stopping = false;
  Lane requests;
  Lane trace_requests;
  DebugTrace debug_trace{trace_requests};
  Lane tool_requests;
  Lane wait_requests;
  std::map<ConnId, std::stop_source> connection_stops;
  std::map<std::pair<ConnId, uint64_t>, std::shared_ptr<std::stop_source>>
      tool_stops;
  Lane terminal_requests;
  // A close must wake an input blocked by terminal backpressure.
  Lane terminal_closes;
  Lane environment_stops;

  auto TraceTargets() -> DebugTrace::Targets {
    DebugTrace::Targets targets;
    auto query = db.prepare(
        "SELECT p.id,p.endpoint,p.token,e.key FROM projects p "
        "LEFT JOIN environment_projects e ON e.project=p.id");
    while (query.step()) {
      auto endpoint = query.text(1);
      std::string capability;
      if (!endpoint.starts_with("unix:")) {
        // Only the managed guest route may carry a capture snapshot over
        // WebSocket. Its broker capability is absent from the project RPC
        // credentials given to browser clients.
        const auto key = query.text(3);
        if (options.vm.binary.empty() || key.empty()) continue;
        capability = fs::read_file(options.environment.directory / key /
                                  "control/execution-broker-capability")
                         .value_or("");
        if (capability.empty()) continue;
      }
      targets.emplace(query.text(0),
                      DebugTrace::Target{std::move(endpoint), query.text(2),
                                         std::move(capability)});
    }
    return targets;
  }

  auto TraceProjectsChanged() -> void {
    if (!trace::LaunchEnabled()) return;
    trace_requests.submit([this] {
      DebugTrace::Targets targets;
      {
        std::lock_guard lock(mutex);
        if (stopping) return;
        targets = TraceTargets();
      }
      debug_trace.Reconcile(std::move(targets));
      debug_trace.CatalogChanged();
    });
  }

  auto EnvironmentChanged(const H::EnvironmentStateT& state) -> void {
    std::lock_guard lock(mutex);
    if (state.phase == H::EnvironmentPhase::Ready) {
      registry.RegisterReady(state);
    } else if (state.phase == H::EnvironmentPhase::Stopped ||
               state.phase == H::EnvironmentPhase::Failed) {
      registry.UnregisterEnvironment(state.key);
    }
    TraceProjectsChanged();
    events->Publish(wire::HostEvent(state));
    if (state.phase == H::EnvironmentPhase::Stopped ||
        state.phase == H::EnvironmentPhase::Failed) {
      H::EnvironmentStoppedT stopped;
      stopped.state = std::make_unique<H::EnvironmentStateT>(state);
      events->Publish(wire::HostEvent(std::move(stopped)));
    }
  }
  auto Scrub(std::string raw) -> std::string {
    std::lock_guard guard(mutex);
    const auto secrets = credentials.Secrets();
    auto plain = [&](std::string text) {
      if (host_settings) text = host_settings->Redact(std::move(text));
      for (const auto& secret : secrets)
        if (!secret.empty())
          for (size_t i = 0; (i = text.find(secret, i)) != text.npos;) {
            text.replace(i, secret.size(), "[redacted]");
            i += 10;
          }
      return text;
    };
    // Preserve protocol keys and enum values; only data strings can contain a
    // copied credential. Replacing bytes in a serialized envelope corrupts it
    // when a deliberately short test key coincides with a field name.
    std::function<std::string(std::string_view)> scrub =
        [&](std::string_view value) -> std::string {
      if (value.starts_with('{')) {
        auto fields = json::Object(value);
        for (auto& [key, data] : fields)
          if (key != "kind" && key != "role" && key != "type")
            data = scrub(data);
        return json::Encode(fields);
      }
      if (value.starts_with('[')) {
        auto rows = json::Array(value);
        for (auto& row : rows) row = scrub(row);
        return json::Encode(rows);
      }
      if (value.starts_with('"'))
        return json::Quote(plain(json::String(value)));
      return std::string(value);
    };
    try {
      if (raw.starts_with('{') || raw.starts_with('[')) return scrub(raw);
    } catch (...) {
    }
    return plain(std::move(raw));
  }
  auto Event(const std::string& id, const std::string& input) -> void {
    const auto raw = Scrub(input);
    std::lock_guard lock(mutex);
    if (turns->running.contains(id)) {
      const auto event = json::Object(raw);
      const auto kind = json::Text(event, "kind");
      turns->ThoughtsFor(id)->Note(
          kind == "helper"
              ? json::Text(json::Object(json::Get(event, "event")), "kind")
              : kind);
    }
    conversations->RecordEvent(id, raw);
  }

  auto Handle(const RequestIn& request, bool human) -> std::string {
    const auto* r = A::GetMessage(request.frame->data())->payload_as_Request();
    std::unique_lock lock(mutex);
    if (stopping) throw Failure("unavailable", "Host service is stopping");
    switch (r->value_type()) {
      case A::RequestPayload::ConfigureRequest: {
        Human(human);
        const auto* p = r->value_as_ConfigureRequest();
        const auto conversation =
            conversations->GetConversation(p->conversation_id()->str());
        auto selection = json::Object(wire::SelectionJson(*p->provider()));
        if (conversation.role == "guide")
          selection["purpose"] = json::Quote("guide");
        const auto raw = json::Encode(selection);
        providers->Configure(p->conversation_id()->str(), raw);
        return wire::AgentReply(request.request_id, S::AcknowledgementT{});
      }
      case A::RequestPayload::ConversationCreateRequest: {
        return turns->ConversationResult(
            request.request_id,
            conversations->Create(*r->value_as_ConversationCreateRequest(),
                                  human));
      }
      case A::RequestPayload::ConversationGetRequest: {
        auto c = conversations->GetConversation(
            r->value_as_ConversationGetRequest()->conversation_id()->str());
        conversations->OwnConversation(c.id, human);
        if (!human) conversations->AuthorizeHistory(c.id);
        return turns->ConversationResult(request.request_id, c);
      }
      case A::RequestPayload::ConversationListRequest: {
        return wire::AgentReply(
            request.request_id,
            conversations->List(*r->value_as_ConversationListRequest(), human));
      }
      case A::RequestPayload::ConversationDeleteRequest: {
        Human(human);
        const auto id =
            r->value_as_ConversationDeleteRequest()->conversation_id()->str();
        conversations->Delete(id);
        for (auto call = tools->file_calls.begin();
             call != tools->file_calls.end();) {
          if (call->first.first != id) {
            ++call;
            continue;
          }
          call->second.stop->request_stop();
          call = tools->file_calls.erase(call);
        }
        turns->thoughts.erase(id);
        return wire::AgentReply(request.request_id, S::AcknowledgementT{});
      }
      case A::RequestPayload::ConversationSearchRequest: {
        return wire::AgentReply(
            request.request_id,
            conversations->Search(*r->value_as_ConversationSearchRequest(),
                                  human));
      }
      case A::RequestPayload::ConversationSendRequest:
      case A::RequestPayload::ConversationActionRequest:
      case A::RequestPayload::ConversationCancelRequest:
        return turns->Handle(request, human, lock);
      case A::RequestPayload::EventsRequest: {
        return wire::AgentReply(
            request.request_id,
            conversations->Events(*r->value_as_EventsRequest(), human));
      }
      case A::RequestPayload::CallIdRequest:
      case A::RequestPayload::ToolCatalogRequest:
      case A::RequestPayload::ToolCallRequest:
        return tools->Handle(request, human, lock);
      case A::RequestPayload::AccessRequest:
      case A::RequestPayload::AccessResolveRequest:
      case A::RequestPayload::AccessRevokeRequest:
      case A::RequestPayload::AccessListRequest:
        return access->Handle(request, human);
      case A::RequestPayload::ConversationSummarizeRequest:
        return turns->Handle(request, human, lock);
      default:
        throw Failure("invalid_request", "Unsupported Agent request");
    }
  }
  auto HandleHost(const RequestIn& request) -> std::string {
    const bool human = true;
    const auto* r = H::GetMessage(request.frame->data())->payload_as_Request();
    std::unique_lock lock(mutex);
    if (stopping) throw Failure("unavailable", "Host service is stopping");
    if (const auto* debug = r->value_as_HostTraceRequest()) {
      if (!trace::LaunchEnabled())
        throw Failure("permission_denied",
                      "Host was started without RPC tracing");
      if (request.websocket)
        throw Failure("permission_denied", "RPC capture requires local host access");
      lock.unlock();
      auto result = debug_trace.Handle(request.conn, *debug, [this] {
        std::lock_guard lock(mutex);
        return TraceTargets();
      });
      return wire::HostReply(request.request_id, std::move(result));
    }
    if (r->value_as_HostActivityRequest()) {
      H::HostActivityT activity;
      activity.busy = !turns->running.empty() || requests.pending() > 1 ||
                      !tool_requests.idle() || !wait_requests.idle() ||
                      !terminal_requests.idle();
      lock.unlock();
      if (!activity.busy && environments) activity.busy = environments->Busy();
      return wire::HostReply(request.request_id, activity);
    }
    if (const auto* format = r->value_as_FileFormatRequest()) {
      auto input = std::unique_ptr<H::FileFormatRequestT>(format->UnPack());
      lock.unlock();
      return wire::HostReply(request.request_id, host::Format(*input));
    }
    if (const auto* input = r->value_as_InputTypesRequest()) {
      const auto project = input->project()->str();
      std::vector<std::string> segments;
      for (const auto* part : *input->card()->segments())
        segments.push_back(json::Quote(part->str()));
      json::Members args{{"card", json::Encode(json::Members{
                                      {"segments", json::Encode(segments)}})}};
      if (input->output_port())
        args["output_port"] = json::Quote(input->output_port()->str());
      if (input->language())
        args["language"] = json::Quote(input->language()->str());
      if (input->at_rev())
        args["at_rev"] = json::Quote(std::to_string(*input->at_rev()));
      lock.unlock();
      return wire::HostReply(request.request_id,
                             tools->InputTypesQuery(project, std::move(args),
                                                    shutdown.get_token()));
    }
    if (r->value_as_HostVersionGraphRequest() ||
        r->value_as_HostVersionDiffRequest())
      return versions->HandleHost(request, human, lock);
    if (const auto* attach = r->value_as_AttachFoldersRequest()) {
      Human(human);
      auto input = std::unique_ptr<H::AttachFoldersRequestT>(attach->UnPack());
      lock.unlock();
      if (!environments)
        throw Failure("unavailable", "Environment service is not running");
      H::AttachedFoldersT result;
      auto state = environments->Attach(
          *input, [&](const auto& ready, const auto& folders) {
            std::lock_guard registration_lock(mutex);
            sql::Transaction transaction(db);
            for (const auto& folder : folders) {
              auto item = std::make_unique<H::FolderAttachmentT>();
              item->folder = folder;
              item->resource = "folder:" + ready.project_id + ":" +
                               crypto::sha256_hex(folder);
              registry.AttachFolder(
                  ready.project_id, item->resource, folder,
                  std::filesystem::path(folder).filename().string());
              result.folders.push_back(std::move(item));
            }
            transaction.commit();
          });
      result.state = std::make_unique<H::EnvironmentStateT>(std::move(state));
      return wire::HostReply(request.request_id, result);
    }
    if (const auto* environment = r->value_as_EnvironmentRequest()) {
      if (environment->action() == H::EnvironmentAction::RemoveFolders &&
          environment->folders()) {
        for (const auto* folder : *environment->folders()) {
          auto resource = db.prepare(
              "SELECT f.id FROM folders f JOIN project_paths p ON "
              "p.id=f.project WHERE p.path=? AND f.path=?");
          resource
              .bind(1, std::filesystem::canonical(environment->project()->str())
                           .string())
              .bind(2, folder->str());
          if (resource.step())
            access->Revoke(resource.text(0), S::Access::Read);
        }
      }
      auto input =
          std::unique_ptr<H::EnvironmentRequestT>(environment->UnPack());
      lock.unlock();
      if (!environments)
        throw Failure("unavailable", "Environment service is not running");
      return wire::HostReply(request.request_id, environments->Handle(*input));
    }
    if (r->value_as_HostRuntimeRequest()) {
      lock.unlock();
      if (!environments)
        throw Failure("unavailable", "Environment service is not running");
      auto input = std::unique_ptr<H::HostRuntimeRequestT>(
          r->value_as_HostRuntimeRequest()->UnPack());
      auto result = environments->Runtime(*input);
      if (!options.vm.binary.empty()) {
        result.vm_resources =
            std::make_unique<H::VMResourcesT>(host_settings->VMResources());
        result.vm_default_resources =
            std::make_unique<H::VMResourcesT>(host::VMDefaultResources());
        result.vm_limits =
            std::make_unique<H::VMResourcesT>(host::VMResourceLimits());
      }
      return wire::HostReply(request.request_id, std::move(result));
    }
    if (const auto* terminal = r->value_as_HostTerminalRequest()) {
      auto input = std::unique_ptr<H::HostTerminalRequestT>(terminal->UnPack());
      if (input->action != H::TerminalAction::Open &&
          (!events->terminals.contains(input->session) ||
           events->terminals.at(input->session) != request.conn))
        throw Failure("permission_denied",
                      "This terminal belongs to another connection");
      lock.unlock();
      auto result = environments->Terminal(*input);
      lock.lock();
      if (input->action == H::TerminalAction::Open)
        events->terminals[result.session] = request.conn;
      if (input->action == H::TerminalAction::Close)
        events->terminals.erase(input->session);
      return wire::HostReply(request.request_id, std::move(result));
    }
    if (host::ExecutionBroker::Handles(r->value_type())) {
      auto* broker = execution_broker.get();
      lock.unlock();
      if (!broker)
        throw Failure("unavailable", "Execution broker is not running");
      return broker->Handle(*r, request.request_id);
    }
    if (r->value_type() == H::RequestPayload::BinaryCachesGetRequest ||
        r->value_type() == H::RequestPayload::BinaryCachesPutRequest) {
      Human(human);
      const auto support = options.vm.support.empty()
                               ? options.state_dir.parent_path()
                               : options.vm.support;
      H::BinaryCachesT result;
      if (const auto* put = r->value_as_BinaryCachesPutRequest()) {
        result.caches_json = put->caches_json()->str();
        host::SaveBinaryCaches(support, result.caches_json);
      } else {
        result.caches_json = host::LoadBinaryCaches(support);
      }
      return wire::HostReply(request.request_id, std::move(result));
    }
    if (r->value_type() == H::RequestPayload::ProjectPermissionsRequest ||
        r->value_type() == H::RequestPayload::ProjectPermissionSetRequest) {
      Human(human);
      return access->HandleHost(*r, request.request_id);
    }
    if (host::Settings::Handles(r->value_type())) {
      Human(human);
      lock.unlock();
      auto reply = host_settings->Handle(*r, request.request_id);
      if (r->value_type() == H::RequestPayload::SettingsUpdateRequest) {
        const auto* result = H::GetMessage(reply.data())->payload_as_Reply();
        const auto* snapshot = result ? result->value_as_Settings() : nullptr;
        lock.lock();
        if (snapshot) {
          H::SettingsChangedT changed;
          changed.profile =
              r->value_as_SettingsUpdateRequest()->profile()->str();
          changed.revision = snapshot->revision();
          events->Publish(wire::HostEvent(std::move(changed)));
        }
      }
      return reply;
    }
    if (host::Guide::Handles(r->value_type()))
      return guide.Handle(*r, request.request_id);
    if (host::LayoutHandles(r->value_type())) {
      try {
        return host::LayoutHandle(*r, request.request_id);
      } catch (const host::LayoutFailure& error) {
        throw Failure("invalid_request", error.what());
      }
    }
    if (host::Projects::Handles(r->value_type()))
      return host_projects.Handle(*r, request.request_id);
    switch (r->value_type()) {
      case H::RequestPayload::IdentifierReserveRequest: {
        const auto* input = r->value_as_IdentifierReserveRequest();
        const auto project = input->project_id()->str(),
                   kind = input->kind()->str();
        registry.Lookup(project);
        if (kind != "note" && kind != "group" && kind != "comment")
          throw Failure("invalid_request", "Unknown annotation kind");
        const auto scope = input->scope() ? input->scope()->str() : "";
        if ((kind == "comment") != !scope.empty())
          throw Failure("invalid_request",
                        "Only comments require a Card scope");
        std::vector<std::string> occupied;
        if (input->occupied())
          for (const auto* id : *input->occupied())
            occupied.push_back(id->str());
        H::IdentifierT result;
        result.id = host::ReserveOrdinal(
            db, "annotation:" + project + ":" + kind + ":" + scope,
            kind == "comment" ? "" : kind + "-", occupied);
        return wire::HostReply(request.request_id, std::move(result));
      }
      case H::RequestPayload::DemoListRequest: {
        H::DemoPathsT result;
        for (const auto& path : json::Array(demo_references.ListFiles()))
          result.paths.push_back(json::String(path));
        return wire::HostReply(request.request_id, std::move(result));
      }
      case H::RequestPayload::DemoReadRequest: {
        Tsugumi::Service::TextResultT result;
        result.text =
            demo_references.Read(r->value_as_DemoReadRequest()->path()->str());
        return wire::HostReply(request.request_id, std::move(result));
      }
      case H::RequestPayload::VersionNoteRequest:
        return versions->HandleHost(request, human, lock);
      case H::RequestPayload::PermissionKindsRequest: {
        const auto* p = r->value_as_PermissionKindsRequest();
        return wire::HostReply(request.request_id,
                               host::PermissionKinds(Text(p->language())));
      }

      case H::RequestPayload::ProjectOperateRequest: {
        Human(human);
        const auto operation = std::unique_ptr<H::ProjectOperateRequestT>(
            r->value_as_ProjectOperateRequest()->UnPack());
        lock.unlock();
        const auto reply = tools->OperateProject(*operation, {"ui", {}, {}, {}},
                                                 shutdown.get_token());
        if (reply.wire.empty())
          throw Failure("unavailable", "operation returned no typed outcome");
        H::ProjectOperationResultT result;
        result.reply.reset(Tsugumi::RPC::GetMessage(reply.wire.data())
                               ->payload_as_Reply()
                               ->UnPack());
        return wire::HostReply(request.request_id, std::move(result));
      }

      case H::RequestPayload::VersionCommentsRequest:
        return versions->HandleHost(request, human, lock);
      case H::RequestPayload::RegisterProjectRequest: {
        Human(human);
        registry.RegisterProject(*r->value_as_RegisterProjectRequest());
        TraceProjectsChanged();
        return wire::HostReply(request.request_id, S::AcknowledgementT{});
      }
      case H::RequestPayload::RegisterFolderRequest: {
        Human(human);
        registry.RegisterFolder(*r->value_as_RegisterFolderRequest());
        return wire::HostReply(request.request_id, S::AcknowledgementT{});
      }
      case H::RequestPayload::ProjectTrustRequest:
      case H::RequestPayload::SkillsListRequest:
        return skills_service->Handle(request, human, lock);
      case H::RequestPayload::ModelsListRequest:
        return models->Handle(request, human, lock);
      default:
        throw Failure("invalid_request", "Unsupported Host request");
    }
  }
};

Service::Service(HostOptions options)
    : state_(std::make_unique<State>(std::move(options))) {}
Service::~Service() { Shutdown(); }
auto Service::Attach(TransportServer* server, TransportServer* host_server)
    -> void {
  state_->server = server;
  state_->events->host_server = host_server;
  state_->debug_trace.Attach(*host_server);
}
auto Service::Shutdown() -> void {
  if (!state_) return;
  std::vector<std::shared_ptr<std::jthread>> jobs;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->stopping) return;
    state_->stopping = true;
    state_->shutdown.request_stop();
    for (const auto& [_, job] : state_->turns->jobs) {
      job->request_stop();
      jobs.push_back(job);
    }
  }
  state_->access->changed.notify_all();
  if (state_->environments) state_->environments->Shutdown();
  if (state_->execution_broker) state_->execution_broker->Stop();
  state_->requests.stop();
  state_->trace_requests.stop();
  state_->debug_trace.Stop();
  trace::SetMode(trace::Mode::Off);
  state_->tool_requests.stop();
  state_->wait_requests.stop();
  state_->terminal_requests.stop();
  state_->terminal_closes.stop();
  state_->environment_stops.stop();
  for (const auto& job : jobs)
    if (job->joinable()) job->join();
  state_->host_settings.reset();
}
auto Service::hello(const RequestIn& request) -> std::pair<std::string, bool> {
  const bool host = H::MessageBufferHasIdentifier(request.frame->data());
  const auto* p = host ? H::GetMessage(request.frame->data())
                             ->payload_as_Request()
                             ->value_as_HelloRequest()
                       : A::GetMessage(request.frame->data())
                             ->payload_as_Request()
                             ->value_as_HelloRequest();
  const auto fail = host ? HostFailed : Failed;
  const bool human =
      crypto::equal_secret(p->token()->str(), state_->options.user_token);
  const bool external =
      !host &&
      crypto::equal_secret(p->token()->str(), state_->options.external_token);
  if (!human && !external)
    return {
        fail(request.request_id, host ? "permission_denied" : "unauthenticated",
             "Invalid service credential", {}),
        false};
  {
    std::lock_guard lock(state_->mutex);
    if (host) {
      if (p->client() != Tsugumi::Trace::Client::Trace)
        state_->events->host_connections.insert(request.conn);
    } else {
      state_->events->principals[request.conn] = human;
      state_->connection_stops.try_emplace(request.conn);
    }
  }
  S::HelloResultT result;
  result.server_id = state_->options.server_id;
  result.principal = human ? "user" : "external_agent";
  return {host ? wire::HostReply(request.request_id, std::move(result))
               : wire::AgentReply(request.request_id, std::move(result)),
          true};
}
auto Service::dispatch(RequestIn request) -> void {
  const bool host = H::MessageBufferHasIdentifier(request.frame->data());
  bool human = true;
  std::shared_ptr<std::stop_source> tool_stop;
  bool tool = false, waiting = false;
  if (!host) {
    std::lock_guard lock(state_->mutex);
    human = state_->events->principals.at(request.conn);
    request.connection_stop =
        state_->connection_stops.at(request.conn).get_token();
    const auto* payload =
        A::GetMessage(request.frame->data())->payload_as_Request();
    if (payload->value_type() == A::RequestPayload::ToolCallCancelRequest) {
      const auto key = std::make_pair(
          request.conn,
          payload->value_as_ToolCallCancelRequest()->request_id());
      if (auto found = state_->tool_stops.find(key);
          found != state_->tool_stops.end())
        found->second->request_stop();
      state_->server->reply(
          request.conn,
          wire::AgentReply(request.request_id, S::AcknowledgementT{}));
      return;
    }
    tool = payload->value_type() == A::RequestPayload::ToolCallRequest;
    if (tool) {
      tool_stop = std::make_shared<std::stop_source>();
      state_->tool_stops.emplace(
          std::make_pair(request.conn, request.request_id), tool_stop);
      const auto name =
          payload->value_as_ToolCallRequest()->name()->string_view();
      waiting = name == "run" || name == "run_status";
    }
  }
  auto* server = host ? state_->events->host_server : state_->server;
  const auto fail = host ? HostFailed : Failed;
  auto task = [this, request, human, host, server, fail, tool_stop]() {
    std::stop_callback disconnected(request.connection_stop, [tool_stop] {
      if (tool_stop) tool_stop->request_stop();
    });
    auto invocation = request;
    if (tool_stop) invocation.connection_stop = tool_stop->get_token();
    std::string bytes;
    try {
      if (tool_stop && tool_stop->stop_requested()) throw Cancelled();
      bytes = host ? state_->HandleHost(invocation)
                   : state_->Handle(invocation, human);
    } catch (const Cancelled&) {
      bytes = fail(request.request_id, "cancelled",
                   "The operation was cancelled", {});
    } catch (const host::ProjectFailure& e) {
      bytes = fail(request.request_id, e.code, state_->Scrub(e.what()), {});
    } catch (const Failure& e) {
      bytes = fail(request.request_id, e.code, state_->Scrub(e.what()),
                   e.request_id);
    } catch (const agent::ProviderError& e) {
      const auto code = e.reason_code();
      bytes = fail(request.request_id, code.empty() ? "invalid_request" : code,
                   state_->Scrub(e.what()), {});
    } catch (const std::exception& e) {
      bytes = fail(request.request_id, "invalid_request",
                   state_->Scrub(e.what()), {});
    }
    if (tool_stop) {
      std::lock_guard lock(state_->mutex);
      state_->tool_stops.erase({request.conn, request.request_id});
    }
    server->reply(request.conn, std::move(bytes));
  };
  const auto* host_request =
      host ? H::GetMessage(request.frame->data())->payload_as_Request()
           : nullptr;
  const auto* terminal =
      host_request ? host_request->value_as_HostTerminalRequest() : nullptr;
  const auto* environment =
      host_request ? host_request->value_as_EnvironmentRequest() : nullptr;
  auto& lane =
      host_request && host_request->value_as_HostTraceRequest()
          ? state_->trace_requests
      : environment && environment->action() == H::EnvironmentAction::Stop
          ? state_->environment_stops
      : terminal && terminal->action() == H::TerminalAction::Close
          ? state_->terminal_closes
      : terminal ? state_->terminal_requests
      : waiting  ? state_->wait_requests
      : tool     ? state_->tool_requests
                 : state_->requests;
  if (!lane.submit(task)) {
    if (tool_stop) {
      std::lock_guard lock(state_->mutex);
      state_->tool_stops.erase({request.conn, request.request_id});
    }
    server->reply(request.conn, fail(request.request_id, "busy",
                                     "Service request queue is full", {}));
  }
}
auto Service::disconnected(ConnId connection) -> void {
  std::lock_guard lock(state_->mutex);
  if (auto stop = state_->connection_stops.find(connection);
      stop != state_->connection_stops.end()) {
    stop->second.request_stop();
    state_->connection_stops.erase(stop);
  }
  state_->events->Disconnected(connection);
}
auto Service::host_disconnected(ConnId connection) -> void {
  state_->events->HostDisconnected(connection);
  if (trace::LaunchEnabled())
    state_->trace_requests.submit(
        [this, connection] { state_->debug_trace.Disconnect(connection); });
}

}  // namespace tsugumi::host
