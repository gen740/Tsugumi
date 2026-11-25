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
  std::filesystem::path root;      // checkout or installed runtime (VERSION)
  std::filesystem::path hostd;
  std::filesystem::path state;     // hostd's state folder
  std::filesystem::path web_root;  // the built page
  std::string application;
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

auto LaunchFromEnvironment(const std::filesystem::path& executable,
                           std::string application = "tsugumi") -> Launch;
/** Starts hostd detached when none runs, else reads the running one. */
auto EnsureHost(const Launch& launch) -> Ready;

/** The Agent socket after a successful Hello: as the user (the command's own
 *  requests) or as the external principal (the Claude Code plugin). */
auto ConnectAgent(const Ready& ready, bool user) -> std::unique_ptr<client::Connection>;

}  // namespace tsugumi::cli
