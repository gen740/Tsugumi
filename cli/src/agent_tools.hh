// The Agent tools a local client reaches as hostd's external principal: the
// project tools of hostd's catalog and the agent_* operations of agent.bfbs.
// The MCP server and the command's tool subcommands share this.
#pragma once

#include <flatbuffers/idl.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "launch.hh"

namespace tsugumi::cli {

struct ToolInfo {
  std::string name;
  std::string description;
  std::string input_schema;  // JSON Schema
};

struct Content {
  std::string text;
  bool error = false;
  std::vector<std::pair<std::string, std::string>> images;  // media type, base64
};

class AgentTools {
 public:
  /** Receives the Agent request a call is waiting on (to cancel it). */
  using Track = std::function<void(const std::shared_ptr<client::Connection>&, uint64_t)>;

  explicit AgentTools(Launch launch);

  /** hostd's project tools, then the agent_* operations. */
  auto Catalog() -> std::vector<ToolInfo>;

  struct Shown {
    std::string path, project_id, url;
    bool shown = false, opened = false;
    bool app = false;  // opened in the Mac app (the command bundled in it)
  };
  /** ShowRequest: makes the folder the project tools work on. */
  auto Show(const std::string& path, bool create, bool browser) -> Shown;
  auto Current() -> std::optional<Shown>;
  struct Closed {
    std::string path;
    bool stopped = false;
    std::string held_by;  // page, session or busy when it keeps running
  };
  /** CloseRequest on the current project: the tools no longer work on it. */
  auto Close() -> std::optional<Closed>;

  /** A project tool (on the current project) or an agent_* operation. */
  auto Call(const std::string& name, const std::string& arguments, const Track& track = {}) -> Content;
  /** An Agent request by its union member name, from JSON; its reply value
   *  as JSON. A Failure reply throws RpcFailure. */
  auto Request(const std::string& request, const std::string& arguments, const Track& track = {}) -> std::string;
  /** Stops a tool call's wait in hostd (not the project run it may have started). */
  static auto Cancel(const std::weak_ptr<client::Connection>& agent, uint64_t request) -> void;

 private:
  auto Agent() -> std::shared_ptr<client::Connection>;

  Launch launch_;
  std::mutex mutex_;
  std::mutex connect_;
  std::mutex codec_;
  flatbuffers::Parser encoder_;
  flatbuffers::Parser decoder_;
  std::shared_ptr<client::Connection> agent_;
  std::shared_ptr<std::atomic<bool>> stale_ = std::make_shared<std::atomic<bool>>(false);
  std::optional<Shown> current_;
};

}  // namespace tsugumi::cli
