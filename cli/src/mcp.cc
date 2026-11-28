// The Claude Code plugin's MCP server: newline-delimited JSON-RPC on stdio.
// It forwards calls to hostd's Agent socket as the external principal. The
// tool catalog, execution, conversations and the person's decisions are hostd's.
#include "mcp.hh"

#include <algorithm>
#include <atomic>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

#include "agent_tools.hh"
#include "launch.hh"
#include "mcp_instructions.hh"
#include "rpc.hh"
#include "tsugumi/util/json_object.hh"

namespace tsugumi::cli {
namespace {
namespace C = tsugumi::client;

constexpr const char* kVersions[] = {"2025-06-18", "2025-03-26", "2024-11-05"};

auto OpenTool() -> std::string {
  return json::Encode(json::Members{
      {"name", json::Quote("tsugumi_open")},
      {"description",
       json::Quote("Opens a Tsugumi project (a folder with tsugumi.json) where the user watches it (a Tsugumi app "
                   "window or web UI page), starting the Tsugumi host when none runs, and makes it the project the "
                   "other tools work on. The first call also returns the rules for working with them.")},
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
                                {"description", json::Quote("Open a window when none is free to show the project: the Tsugumi app for the command bundled in it, else a browser (default true).")}})}})},
           {"required", json::Encode(std::vector<std::string>{json::Quote("folder")})},
           {"additionalProperties", "false"}})}});
}

auto CloseTool() -> std::string {
  return json::Encode(json::Members{
      {"name", json::Quote("tsugumi_close")},
      {"description",
       json::Quote("Closes the project tsugumi_open opened. Its environment stops, freeing its place in the shared "
                   "VM, unless a window shows it or a run is going on.")},
      {"inputSchema", R"({"type":"object","properties":{},"additionalProperties":false})"}});
}

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
  explicit Server(Launch launch) : tools_(std::move(launch)) {}

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
    for (const auto& [_, call] : calls_) AgentTools::Cancel(call.agent, call.request);
    return 0;
  }

 private:
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
      if (auto found = calls_.find(cancelled); found != calls_.end())
        AgentTools::Cancel(found->second.agent, found->second.request);
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

  auto Tools() -> std::string {
    // A client resends this list every turn. Of the Agent operations it lists
    // what an outside agent reads about the person's side (decisions and
    // conversations); the others stay callable and are `tsugumi agent` commands.
    constexpr std::string_view kListed[] = {"agent_decision_list", "agent_conversation_list",
                                            "agent_conversation_get"};
    std::vector<std::string> tools = {OpenTool(), CloseTool()};
    for (const auto& tool : tools_.Catalog())
      if (!tool.name.starts_with("agent_") || std::ranges::find(kListed, tool.name) != std::end(kListed))
        tools.push_back(json::Encode(json::Members{{"name", json::Quote(tool.name)},
                                                 {"description", json::Quote(tool.description)},
                                                 {"inputSchema", tool.input_schema}}));
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
    const bool browser = json::Bool(json::Get(a, "browser"), true);
    AgentTools::Shown o;
    try {
      o = tools_.Show(dir.string(), create, browser);
    } catch (const RpcFailure& e) {
      if (e.code == "invalid_project" && !create)
        return {std::string(e.what()) + " Pass create: true to make a new project there.", true, {}};
      throw;
    }
    std::string where = "An open Tsugumi window or page shows it now.";
    if (!o.shown)
      where = o.app      ? "The Tsugumi app opens it in a window."
              : o.opened ? "A browser window opens it."
              : !browser ? "No Tsugumi window or page shows it (one on another project keeps that project, "
                           "and browser: false opens none); the person can open it in the Tsugumi app or with "
                           "`tsugumi web`. The project tools work on it either way."
                         : "No Tsugumi window or page is free to show it (one on another project keeps that "
                           "project); the person can open it in the Tsugumi app or with `tsugumi web`. The "
                           "project tools work on it either way.";
    auto text = "Opened " + o.path + " in Tsugumi" + (o.url.empty() ? "" : " (web UI: " + o.url + ")") + ". " +
                where;
    if (!rules_sent_.exchange(true)) text += "\n\n" + std::string(kInstructions);
    return {text, false, {}};
  }

  auto Close() -> Content {
    const auto closed = tools_.Close();
    if (!closed) return {"No project is open.", false, {}};
    const auto& by = closed->held_by;
    return {"Closed " + closed->path + (closed->stopped ? "; its environment stopped." :
                                        by == "page"    ? "; a Tsugumi window or page still shows it." :
                                        by == "session" ? "; another session still has it open." :
                                                          "; its environment keeps running until its run ends."),
            false,
            {}};
  }

  auto Call(const std::string& mcp_id, const std::string& name, const std::string& args) -> Content {
    if (name == "tsugumi_open") return Open(args);
    if (name == "tsugumi_close") return Close();
    return tools_.Call(name, args, [&](const std::shared_ptr<C::Connection>& agent, uint64_t request) {
      std::lock_guard lock(mutex_);
      calls_[mcp_id] = Pending{agent, request};
    });
  }

  struct Pending {
    std::weak_ptr<C::Connection> agent;
    uint64_t request;
  };

  AgentTools tools_;
  std::mutex mutex_;
  std::mutex output_;
  std::atomic<bool> rules_sent_{false};
  std::map<std::string, Pending> calls_;
};

}  // namespace

auto RunMcp(const std::filesystem::path& executable) -> int {
  Server server(LaunchFromEnvironment(executable, "tsugumi mcp"));
  return server.Run();
}

}  // namespace tsugumi::cli
