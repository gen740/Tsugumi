// One way for every local caller (the tsugumi command, the Claude Code
// plugin's MCP server) to reach the same hostd: the launch arguments come
// from the same environment and defaults, and `tsugumi-hostd --ensure`
// either becomes the host or reports the one that already runs.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "tsugumi/client/client.hh"

namespace tsugumi::cli {

struct Launch {
  std::filesystem::path root;      // checkout or installed runtime (VERSION, demos)
  std::filesystem::path hostd;
  std::filesystem::path state;     // hostd's state folder
  std::filesystem::path web_root;  // the built page
  int web_port = 0;                // 0: the usual one, or any free port
  std::vector<std::string> runtime;  // native binaries, or the shared Mac VM
};

/** host.ready: what hostd publishes to local clients of this user. */
struct Ready {
  std::string unix_socket;  // Agent RPC
  std::string host_socket;  // Host RPC
  std::string user_token;
  std::string external_token;
  long pid = 0;
};

auto LaunchFromEnvironment(const std::filesystem::path& executable) -> Launch;
/** Starts hostd detached when none runs, else reads the running one. */
auto EnsureHost(const Launch& launch) -> Ready;

/** A connection after a successful Hello. */
auto ConnectHost(const Ready& ready) -> std::unique_ptr<client::Connection>;
auto ConnectAgent(const Ready& ready) -> std::unique_ptr<client::Connection>;

struct Web {
  std::string url;       // the origin
  std::string page_url;  // carries the page key
};
/** Serves the Web UI (when it does not yet) and optionally opens a page. */
auto ServeWeb(client::Connection& host, const Launch& launch, bool open,
              const std::string& project) -> Web;

}  // namespace tsugumi::cli
