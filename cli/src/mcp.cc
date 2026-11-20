// The Claude Code plugin's MCP server: newline-delimited JSON-RPC on stdio.
// It forwards calls to hostd's Agent socket as the external principal. The
// tool catalog, execution, conversations and access decisions are hostd's.
#include "mcp.hh"

#include <flatbuffers/idl.h>

#include <atomic>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

#include "agent_catalog.hh"
#include "launch.hh"
#include "mcp_instructions.hh"
#include "rpc.hh"
#include "tsugumi/util/json_object.hh"

namespace tsugumi::cli {
namespace {
namespace A = Tsugumi::Agent;
namespace C = tsugumi::client;

constexpr const char* kVersions[] = {"2025-06-18", "2025-03-26", "2024-11-05"};

auto OpenTool() -> std::string {
  return json::Encode(json::Members{
      {"name", json::Quote("tsugumi_open")},
      {"description",
       json::Quote("Opens a Tsugumi project (a folder with tsugumi.json) in the Tsugumi web UI, starting the web UI "
                   "host when none runs, and makes it the project the other tools work on. The first call also "
                   "returns the rules for working with them.")},
      {"inputSchema",
       json::Encode(json::Members{
           {"type", json::Quote("object")},
           {"properties",
            json::Encode(json::Members{
                {"folder", json::Encode(json::Members{
                               {"type", json::Quote("string")},
                               {"description", json::Quote("The project folder (absolute, or relative to the Claude Code project).")}})},
                {"create", json::Encode(json::Members{
                               {"type", json::Quote("boolean")},
                               {"description", json::Quote("Make a new empty project in this folder when it has no tsugumi.json (the folder must not exist yet).")}})},
                {"browser", json::Encode(json::Members{
                                {"type", json::Quote("boolean")},
                                {"description", json::Quote("Open a browser window when no web UI page is open (default true).")}})}})},
           {"required", json::Encode(std::vector<std::string>{json::Quote("folder")})},
           {"additionalProperties", "false"}})}});
}

struct Content {
  std::string text;
  bool error = false;
  std::vector<std::pair<std::string, std::string>> images;  // media type, base64
};

auto ResultJson(const Content& c) -> std::string {
  std::vector<std::string> content = {
      json::Encode(json::Members{{"type", json::Quote("text")}, {"text", json::Quote(c.text)}})};
  for (const auto& [type, data] : c.images)
    content.push_back(json::Encode(
        json::Members{{"type", json::Quote("image")}, {"data", json::Quote(data)}, {"mimeType", json::Quote(type)}}));
  json::Members out{{"content", json::Encode(content)}};
  if (c.error) out["isError"] = "true";
  return json::Encode(out);
}

class Server {
 public:
  explicit Server(Launch launch) : launch_(std::move(launch)) {
    const auto [schema, size] = AgentSchema();
    encoder_.opts.strict_json = true;
    decoder_.opts.strict_json = true;
    decoder_.opts.output_default_scalars_in_json = true;
    decoder_.opts.indent_step = -1;
    if (!encoder_.Deserialize(schema, size) || !decoder_.Deserialize(schema, size))
      throw std::runtime_error("cannot load the Agent schema");
  }

  auto Run() -> int {
    std::string line;
    while (std::getline(std::cin, line)) {
      if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
      json::Members message;
      try {
        message = json::Object(line);
      } catch (...) {
        Reply("null", {}, R"({"code":-32700,"message":"parse error"})");
        continue;
      }
      Handle(std::move(message));
    }
    // stdin closed: Claude Code ended. In-flight calls end with the process.
    std::lock_guard lock(mutex_);
    for (const auto& [_, call] : calls_) Cancel(call);
    return 0;
  }

 private:
  struct Project {
    std::string path;
    std::string id;
  };

  auto Reply(const std::string& id, const std::string& result, const std::string& error = {}) -> void {
    json::Members out{{"jsonrpc", json::Quote("2.0")}, {"id", id}};
    if (!error.empty())
      out["error"] = error;
    else
      out["result"] = result;
    std::lock_guard lock(output_);
    std::cout << json::Encode(out) << '\n' << std::flush;
  }

  auto Failure(int code, const std::string& message) -> std::string {
    return json::Encode(json::Members{{"code", std::to_string(code)}, {"message", json::Quote(message)}});
  }

  auto Handle(json::Members m) -> void {
    const auto id = json::Get(m, "id");
    const bool request = id != "null";
    try {
      Dispatch(m, id, request);
    } catch (const std::exception& e) {
      // Malformed params: an error for a request, nothing for a notification.
      if (request) Reply(id, {}, Failure(-32602, std::string("invalid params: ") + e.what()));
    }
  }

  auto Dispatch(const json::Members& m, const std::string& id, bool request) -> void {
    const auto method = json::Text(m, "method");
    const auto params = json::Get(m, "params", "{}");
    if (method == "notifications/cancelled") {
      const auto cancelled = json::Get(json::Object(params), "requestId");
      std::lock_guard lock(mutex_);
      if (auto found = calls_.find(cancelled); found != calls_.end()) Cancel(found->second);
      return;
    }
    if (method == "initialize") {
      const auto asked = json::Text(json::Object(params), "protocolVersion");
      std::string version = kVersions[0];
      for (const auto* v : kVersions)
        if (asked == v) version = v;
      return Reply(id, json::Encode(json::Members{
                           {"protocolVersion", json::Quote(version)},
                           {"capabilities", R"({"tools":{}})"},
                           {"serverInfo", R"({"name":"tsugumi","version":"0"})"},
                           {"instructions", json::Quote(kInitialInstructions)}}));
    }
    if (method == "ping") return Reply(id, "{}");
    if (method == "tools/list") {
      // Listing may start the host; the stdin loop keeps reading meanwhile.
      std::thread([this, id] {
        try {
          Reply(id, json::Encode(json::Members{{"tools", Tools()}}));
        } catch (const std::exception& e) {
          Reply(id, {}, Failure(-32603, e.what()));
        }
      }).detach();
      return;
    }
    if (method == "tools/call") {
      // Each call runs on its own; a run that waits must not hold the others.
      std::thread([this, id, params] {
        Content result;
        try {
          const auto p = json::Object(params);
          result = Call(id, json::Text(p, "name"), json::Get(p, "arguments", "{}"));
        } catch (const std::exception& e) {
          result = {e.what(), true, {}};
        }
        {
          std::lock_guard lock(mutex_);
          calls_.erase(id);
        }
        Reply(id, ResultJson(result));
      }).detach();
      return;
    }
    // Other notifications need no answer.
    if (request) Reply(id, {}, Failure(-32601, "method not found: " + method));
  }

  /** The host's Agent socket, starting the host when none runs. Starting
   *  waits on connect_ alone: cancellations and pings keep being answered. */
  auto Agent() -> std::shared_ptr<C::Connection> {
    std::lock_guard connecting(connect_);
    {
      std::lock_guard lock(mutex_);
      if (agent_ && !stale_->load()) return agent_;
    }
    // A new host (after a restart) or a closed socket: connect again.
    auto ready = EnsureHost(launch_);
    // A host started now also serves the page; one that ran already is
    // asked to (the desktop app's host serves no page until then).
    auto host = ConnectHost(ready);
    ServeWeb(*host, launch_, false, {});
    auto stale = std::make_shared<std::atomic<bool>>(false);
    auto agent = std::shared_ptr<C::Connection>(ConnectAgent(ready));
    agent->OnClosed([stale](C::Error) { *stale = true; });
    std::lock_guard lock(mutex_);
    agent_ = agent;
    stale_ = stale;
    return agent;
  }

  auto Tools() -> std::string {
    std::vector<std::string> tools = {OpenTool()};
    const auto catalog = AgentCall<A::ToolCatalog>(*Agent(), A::ToolCatalogRequestT{});
    if (catalog.view->tools())
      for (const auto* tool : *catalog.view->tools())
        tools.push_back(json::Encode(json::Members{
            {"name", json::Quote(tool->name()->str())},
            {"description", json::Quote(tool->description() ? tool->description()->str() : "")},
            {"inputSchema", tool->input_schema_json() ? tool->input_schema_json()->str() : "{}"}}));
    for (const auto& op : AgentOperations())
      tools.push_back(json::Encode(json::Members{{"name", json::Quote(op.tool)},
                                                 {"description", json::Quote(op.description)},
                                                 {"inputSchema", op.input_schema}}));
    return json::Encode(tools);
  }

  auto Open(const std::string& args) -> Content {
    const auto a = json::Object(args);
    const auto folder = json::Text(a, "folder");
    if (folder.find_first_not_of(" \t") == std::string::npos) return {"folder is required", true, {}};
    const auto* base = std::getenv("CLAUDE_PROJECT_DIR");
    const auto dir = (std::filesystem::path(base ? base : std::filesystem::current_path().string()) / folder)
                         .lexically_normal();
    const bool create = json::Bool(json::Get(a, "create"));
    A::ProjectOpenRequestT request;
    request.path = dir.string();
    request.create = create;
    request.browser = json::Bool(json::Get(a, "browser"), true);
    std::optional<cli::Reply<A::ProjectOpened>> opened;
    try {
      opened = AgentCall<A::ProjectOpened>(*Agent(), std::move(request));
    } catch (const RpcFailure& e) {
      if (e.code == "invalid_project" && !create)
        return {std::string(e.what()) + " Pass create: true to make a new project there.", true, {}};
      throw;
    }
    const auto* o = opened->view;
    {
      std::lock_guard lock(mutex_);
      current_ = Project{o->path()->str(), o->project_id()->str()};
    }
    std::string where = "The open web UI page shows it now.";
    if (!o->shown())
      where = o->opened() ? "A browser window opens it."
                          : "No web UI page is free to show it (a page on another project keeps that project); the "
                            "user opens it from the tsugumi command's URL (or call tsugumi_open again without "
                            "browser: false).";
    auto text = "Opened " + o->path()->str() + " in the Tsugumi web UI (" + (o->url() ? o->url()->str() : "") +
                "). " + where + " Opening a project does not grant Agent access to its contents.";
    if (!rules_sent_.exchange(true)) text += "\n\n" + std::string(kInstructions);
    return {text, false, {}};
  }

  auto Call(const std::string& mcp_id, const std::string& name, const std::string& args) -> Content {
    if (name == "tsugumi_open") return Open(args);
    if (name.starts_with("agent_")) return Operation(mcp_id, name, args);
    std::optional<Project> project;
    {
      std::lock_guard lock(mutex_);
      project = current_;
    }
    if (!project) return {"No Tsugumi project is open: call tsugumi_open with its folder first.", true, {}};
    const auto connection = Agent();
    auto& agent = *connection;
    const auto conversation = "external-" + project->id;
    // A moved transcript needs fresh grants. The tool request asks hostd for
    // its own access without reading that transcript.
    try {
      A::ConversationCreateRequestT create;
      create.conversation_id = conversation;
      create.project_id = project->id;
      create.role = "external";
      AgentCall<A::ConversationResult>(agent, std::move(create));
    } catch (const RpcFailure& e) {
      if (e.code != "permission_denied") throw;
    }
    A::CallIdRequestT reserve;
    reserve.conversation_id = conversation;
    reserve.name = name;
    reserve.arguments_json = args;
    const auto op = AgentCall<A::CallId>(agent, std::move(reserve)).view->id()->str();
    A::ToolCallRequestT call;
    call.conversation_id = conversation;
    call.project_id = project->id;
    call.name = name;
    call.arguments_json = args;
    call.op_id = op;
    try {
      const auto result =
          AgentCall<A::ToolResult>(agent, std::move(call), [&](uint64_t request) { Track(mcp_id, connection, request); });
      Content out{result.view->text() ? result.view->text()->str() : "", result.view->is_error(), {}};
      if (result.view->images())
        for (const auto* image : *result.view->images())
          out.images.emplace_back(image->media_type()->str(), image->data()->str());
      return out;
    } catch (const RpcFailure& e) {
      if (e.code != "grant_required" || e.request_id.empty()) throw;
      return {json::Encode(json::Members{{"code", json::Quote(e.code)},
                                         {"message", json::Quote(e.what())},
                                         {"request_id", json::Quote(e.request_id)}}),
              true,
              {}};
    }
  }

  /** An agent_* tool: the Agent RPC request of that name, from JSON. */
  auto Operation(const std::string& mcp_id, const std::string& name, const std::string& args) -> Content {
    const AgentOperation* operation = nullptr;
    for (const auto& op : AgentOperations())
      if (op.tool == name) operation = &op;
    if (!operation) throw std::runtime_error("Unknown Agent operation: " + name);
    const auto connection = Agent();
    auto& agent = *connection;
    const auto frame = [&](uint64_t id) {
      const auto text = "{\"request_id\":" + std::to_string(id) +
                        ",\"payload_type\":\"Request\",\"payload\":{\"value_type\":" +
                        json::Quote(operation->request) + ",\"value\":" + args + "}}";
      std::lock_guard lock(codec_);
      if (!encoder_.ParseJson(text.c_str())) throw std::runtime_error("invalid arguments: " + encoder_.error_);
      const auto* begin = encoder_.builder_.GetBufferPointer();
      return C::Bytes(begin, begin + encoder_.builder_.GetSize());
    };
    frame(1);  // the arguments are checked before anything is sent
    auto result = agent
                      .Call([&](uint64_t id) {
                        Track(mcp_id, connection, id);
                        return frame(id);
                      })
                      .get();
    if (result.error) throw std::runtime_error("Tsugumi host connection: " + result.error.message);
    std::string text;
    {
      std::lock_guard lock(codec_);
      if (const auto* failed = flatbuffers::GenerateText(decoder_, result.bytes.data(), &text))
        throw std::runtime_error(std::string("cannot read the reply: ") + failed);
    }
    const auto payload = json::Object(json::Get(json::Object(text), "payload", "{}"));
    const auto value = json::Get(payload, "value", "{}");
    if (json::Text(payload, "value_type") == "Failure") {
      const auto failure = json::Object(value);
      throw RpcFailure(json::Text(failure, "code"), json::Text(failure, "message"), json::Text(failure, "request_id"));
    }
    return {value, false, {}};
  }

  /** Records the Agent request that notifications/cancelled stops. */
  auto Track(const std::string& mcp_id, const std::shared_ptr<C::Connection>& agent, uint64_t request) -> void {
    std::lock_guard lock(mutex_);
    calls_[mcp_id] = Pending{agent, request};
  }

  struct Pending {
    std::weak_ptr<C::Connection> agent;
    uint64_t request;
  };
  /** Stops the call's wait in hostd (not the project run it may have started). */
  auto Cancel(const Pending& call) -> void {
    const auto agent = call.agent.lock();
    if (!agent) return;
    A::ToolCallCancelRequestT cancel;
    cancel.request_id = call.request;
    agent->Call(
        [cancel](uint64_t id) {
          A::RequestT request;
          request.value.Set(A::ToolCallCancelRequestT(cancel));
          A::MessageT message;
          message.request_id = id;
          message.payload.Set(std::move(request));
          flatbuffers::FlatBufferBuilder b;
          A::FinishMessageBuffer(b, A::Message::Pack(b, &message));
          return C::Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
        },
        [](C::Result) {});
  }

  Launch launch_;
  std::mutex mutex_;
  std::mutex connect_;
  std::mutex output_;
  std::mutex codec_;
  flatbuffers::Parser encoder_;
  flatbuffers::Parser decoder_;
  std::shared_ptr<C::Connection> agent_;
  std::shared_ptr<std::atomic<bool>> stale_ = std::make_shared<std::atomic<bool>>(false);
  std::optional<Project> current_;
  std::atomic<bool> rules_sent_{false};
  std::map<std::string, Pending> calls_;
};

}  // namespace

auto RunMcp(const std::filesystem::path& executable) -> int {
  Server server(LaunchFromEnvironment(executable));
  return server.Run();
}

}  // namespace tsugumi::cli
