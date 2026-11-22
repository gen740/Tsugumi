#include "tsugumi/cli/launch.hh"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <random>
#include <stdexcept>

#include "tsugumi/cli/rpc.hh"
#include "tsugumi/protocol/agent_generated.h"
#include "tsugumi/protocol/host_generated.h"
#include "tsugumi/util/json_object.hh"

namespace tsugumi::cli {
namespace f = std::filesystem;
namespace {

auto Env(const char* name) -> std::string {
  const auto* value = std::getenv(name);
  return value ? value : "";
}

auto Home() -> f::path {
  const auto home = Env("HOME");
  if (home.empty()) throw std::runtime_error("HOME is not set");
  return home;
}

auto FirstExisting(const std::vector<f::path>& candidates) -> f::path {
  for (const auto& candidate : candidates)
    if (f::exists(candidate)) return candidate;
  return candidates.front();
}

auto RandomName() -> std::string {
  std::random_device device;
  std::string out;
  for (int i = 0; i < 12; ++i) out += "0123456789abcdef"[device() % 16];
  return out;
}

}  // namespace

auto LaunchFromEnvironment(const f::path& executable) -> Launch {
  Launch launch;
  const auto self = f::weakly_canonical(executable);
  // A checkout's build/cli/tsugumi, or an installed runtime's bin/tsugumi.
  launch.root = !Env("TSUGUMI_REPO").empty() ? f::path(Env("TSUGUMI_REPO"))
                                              : self.parent_path().parent_path();
  if (!f::exists(launch.root / "VERSION") && f::exists(launch.root.parent_path() / "VERSION"))
    launch.root = launch.root.parent_path();
  const auto build = !Env("TSUGUMI_BUILD_DIR").empty() ? f::path(Env("TSUGUMI_BUILD_DIR"))
                                                        : launch.root / "build";
  launch.hostd = !Env("TSUGUMI_HOSTD").empty()
                     ? f::path(Env("TSUGUMI_HOSTD"))
                     : FirstExisting({self.parent_path() / "tsugumi-hostd", launch.root / "bin" / "tsugumi-hostd",
                                      build / "hostd" / "tsugumi-hostd"});
  launch.web_root = !Env("TSUGUMI_WEB_ROOT").empty() ? f::path(Env("TSUGUMI_WEB_ROOT"))
                                                      : launch.root / "ui" / "web" / "dist" / "public";
  if (!Env("TSUGUMI_WEB_PORT").empty()) launch.web_port = std::stoi(Env("TSUGUMI_WEB_PORT"));
#ifdef __APPLE__
  const auto support = !Env("TSUGUMI_SUPPORT_DIR").empty()
                           ? f::path(Env("TSUGUMI_SUPPORT_DIR"))
                           : Home() / "Library" / "Application Support" / "Tsugumi";
  const auto data = !Env("TSUGUMI_WEB_DATA").empty() ? f::path(Env("TSUGUMI_WEB_DATA")) : support / "web";
  // The desktop app's host: the same VM, store, settings and recent projects.
  launch.state = support / "host-core";
#else
  const auto xdg = !Env("XDG_DATA_HOME").empty() ? f::path(Env("XDG_DATA_HOME")) : Home() / ".local" / "share";
  const auto data = !Env("TSUGUMI_WEB_DATA").empty() ? f::path(Env("TSUGUMI_WEB_DATA")) : xdg / "tsugumi-web";
  launch.state = data / "host-core";
#endif
  if (!Env("TSUGUMI_HOST_STATE").empty()) launch.state = f::absolute(Env("TSUGUMI_HOST_STATE"));
  if (!Env("TSUGUMI_VMHOST").empty()) {
    // The shared Mac VM's helper (a stand-in for it in checks).
    const auto vm_support = !Env("TSUGUMI_SUPPORT_DIR").empty() ? f::path(Env("TSUGUMI_SUPPORT_DIR")) : data.parent_path();
    launch.runtime = {"--vmhost", Env("TSUGUMI_VMHOST"), "--vm-support", vm_support.string(),
                      "--vm-image", Env("TSUGUMI_VM_IMAGE")};
  } else {
    const auto pick = [&](const char* name, const f::path& fallback) {
      return !Env(name).empty() ? Env(name) : fallback.string();
    };
    launch.runtime = {"--tsugumid", pick("TSUGUMID", FirstExisting({self.parent_path() / "tsugumid", build / "tsugumid" / "tsugumid"})),
                      "--worker", pick("TSUGUMI_WORKER", FirstExisting({self.parent_path() / "tsugumi-worker", build / "workerd" / "tsugumi-worker"})),
                      "--environment-dir", (data / "env").string()};
    if (const auto dir = Env("TSUGUMI_NIX_DIR"); !dir.empty())
      launch.runtime.insert(launch.runtime.end(), {"--nix-dir", dir});
  }
  return launch;
}

auto EnsureHost(const Launch& launch) -> Ready {
  if (!f::exists(launch.hostd))
    throw std::runtime_error("tsugumi-hostd not found: " + launch.hostd.string() + " (build the core first)");
  // Each start gets a short private socket folder; the host keeps its own
  // stable broker folder.
  const f::path sockets_root = !Env("TSUGUMI_SOCKET_DIR").empty() ? f::path(Env("TSUGUMI_SOCKET_DIR")) : f::path("/tmp");
  const auto sockets = sockets_root / ("tsugumi-host-" + RandomName());
  if (::mkdir(sockets.c_str(), 0700) != 0)
    throw std::runtime_error("cannot create " + sockets.string() + ": " + std::strerror(errno));
  // hostd creates and numbers its state folder. Until it published
  // readiness, its log lives beside the state folder.
  const auto parent = launch.state.parent_path();
  f::create_directories(parent);
  ::chmod(parent.c_str(), 0700);
  const auto state_log = launch.state / "host.log";
  const auto log_path = f::exists(launch.state) ? state_log : parent / (".host-startup-" + RandomName() + ".log");
  std::vector<std::string> args = {launch.hostd.string(), "--ensure", "--state-dir", launch.state.string(),
                                   "--unix", (sockets / "agent.sock").string(),
                                   "--demos-dir", (launch.root / "demos").string()};
  args.insert(args.end(), launch.runtime.begin(), launch.runtime.end());
  if (f::exists(launch.web_root / "index.html")) {
    args.insert(args.end(), {"--web-root", launch.web_root.string()});
    if (launch.web_port) args.insert(args.end(), {"--web-port", std::to_string(launch.web_port)});
  }
  int out[2];
  if (::pipe(out) != 0) throw std::runtime_error("pipe failed");
  const int log = ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  if (log < 0) throw std::runtime_error("cannot open " + log_path.string());
  const pid_t child = ::fork();
  if (child < 0) throw std::runtime_error("fork failed");
  if (child == 0) {
    // Its own session: the host outlives this command and its terminal.
    ::setsid();
    const int null = ::open("/dev/null", O_RDONLY);
    ::dup2(null, 0);
    ::dup2(out[1], 1);
    ::dup2(log, 2);
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    ::execv(argv[0], argv.data());
    _exit(127);
  }
  ::close(out[1]);
  ::close(log);
  std::string line;
  char c;
  while (::read(out[0], &c, 1) == 1 && c != '\n') line += c;
  ::close(out[0]);
  if (line.empty()) {
    ::waitpid(child, nullptr, 0);
    f::remove(sockets);
    throw std::runtime_error("the Tsugumi host did not start; see " + log_path.string());
  }
  const auto value = json::Object(line);
  Ready ready;
  ready.unix_socket = json::Text(value, "unix_socket");
  ready.host_socket = json::Text(value, "host_socket");
  ready.user_token = json::Text(value, "user_token");
  ready.external_token = json::Text(value, "external_token");
  ready.pid = static_cast<long>(json::UInt(json::Get(value, "pid", "0")));
  if (ready.pid == child) {
    if (log_path != state_log) f::rename(log_path, state_log);
  } else {
    // Another host runs: this child only reported it.
    ::waitpid(child, nullptr, 0);
    f::remove(sockets);
    std::error_code ignored;
    if (log_path != state_log && f::file_size(log_path, ignored) == 0) f::remove(log_path, ignored);
  }
  return ready;
}

auto ConnectAgent(const Ready& ready, bool user) -> std::unique_ptr<client::Connection> {
  client::Options options;
  options.protocol = client::Protocol::Agent;
  options.endpoint = "unix:" + ready.unix_socket;
  auto connection = std::make_unique<client::Connection>(options);
  if (auto error = connection->Start().get()) throw std::runtime_error(error.message);
  Tsugumi::Service::HelloRequestT hello;
  hello.token = user ? ready.user_token : ready.external_token;
  AgentCall<Tsugumi::Service::HelloResult>(*connection, std::move(hello));
  return connection;
}

}  // namespace tsugumi::cli
