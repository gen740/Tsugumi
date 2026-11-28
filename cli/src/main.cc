// The tsugumi command: a local client of hostd's RPC. A host it starts keeps
// running after the command ends, like the desktop app's.
//
//   tsugumi <tool> [--option value ...]
//                                one Agent tool (the MCP server's catalog), JSON out
//   tsugumi call <Request> [json]  one raw Agent request (debugging)
//   tsugumi web [--port N] [--no-browser] [project folder]
//                                shows the Web UI (hostd serves it)
//   tsugumi mcp                  the Claude Code plugin's MCP server (stdio)
//   tsugumi update               puts the latest release in place
//   tsugumi uninstall [--yes]    previews removal; --yes removes the command
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "commands.hh"
#include "launch.hh"
#include "maintenance.hh"
#include "mcp.hh"
#include "rpc.hh"

namespace {
namespace f = std::filesystem;
using namespace tsugumi::cli;

auto Executable(const char* argv0) -> f::path {
#ifdef __APPLE__
  char buffer[4096];
  uint32_t size = sizeof buffer;
  if (_NSGetExecutablePath(buffer, &size) == 0) return f::canonical(buffer);
#else
  std::error_code error;
  if (auto self = f::read_symlink("/proc/self/exe", error); !error) return self;
#endif
  return f::absolute(argv0);
}

auto Usage() -> void {
  std::cout << "Usage: tsugumi <tool> [--option value ...] [--json '<object>'] "
               "[--project <folder>]\n"
               "       tsugumi lock update [name] [--project <folder>] "
               "[--project-path <external>]\n"
               "       tsugumi host-path list PROJECT\n"
               "       tsugumi host-path bind PROJECT NAME FOLDER\n"
               "       tsugumi host-path unbind PROJECT NAME\n"
               "       tsugumi call <RequestName> ['<json>']\n"
               "       tsugumi web [--port N] [--no-browser] [project folder]\n"
               "       tsugumi mcp\n"
               "       tsugumi update | uninstall [--yes] | --version\n"
               "Tools work as the host's Agent principal, with the same access "
               "as Claude Code's.\n";
}

auto HostPaths(const f::path& executable, const std::vector<std::string>& args)
    -> int {
  namespace H = Tsugumi::Host;
  const auto action = args.size() > 1 ? args[1] : "";
  if (!((action == "list" && args.size() == 3) ||
        (action == "unbind" && args.size() == 4) ||
        (action == "bind" && args.size() == 5))) {
    Usage();
    return 64;
  }
  const auto folder = f::canonical(args[2]);
  std::string project;
  std::ifstream(folder / ".tsugumi/project-id") >> project;
  if (project.empty())
    throw std::runtime_error(
        "Open this project in Tsugumi to create its project id");
  const auto ready =
      EnsureHost(LaunchFromEnvironment(executable, "tsugumi host-path"));
  tsugumi::client::Options options;
  options.protocol = tsugumi::client::Protocol::Host;
  options.endpoint = "unix:" + ready.host_socket;
  tsugumi::client::Connection connection(options);
  if (auto error = connection.Start().get())
    throw std::runtime_error(error.message);
  Tsugumi::Service::HelloRequestT hello;
  hello.token = ready.user_token;
  HostCall<Tsugumi::Service::HelloResult>(connection, std::move(hello));
  auto list = [&] {
    if (action == "list") {
      H::HostPathListRequestT request;
      request.project_id = project;
      return HostCall<H::HostPathBindings>(connection, std::move(request));
    }
    H::HostPathSetRequestT request;
    request.project_id = project;
    const auto slash = args[3].rfind('/');
    if (slash != std::string::npos)
      request.scope = args[3].substr(0, slash + 1);
    request.name = args[3].substr(slash == std::string::npos ? 0 : slash + 1);
    request.remove = action == "unbind";
    if (!request.remove) request.path = f::absolute(args[4]).string();
    return HostCall<H::HostPathBindings>(connection, std::move(request));
  }();
  if (const auto* bindings = list.view->bindings())
    for (const auto* entry : *bindings)
      std::cout << (entry->scope() ? entry->scope()->str() : "")
                << entry->name()->str() << '\t' << entry->path()->str() << '\n';
  return 0;
}

auto Open(const f::path& executable, const std::vector<std::string>& args) -> int {
  auto launch = LaunchFromEnvironment(executable, "tsugumi web");
  bool browser = true;
  std::string project;
  for (size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--port" && i + 1 < args.size())
      launch.web_port = std::stoi(args[++i]);
    else if (args[i] == "--no-browser")
      browser = false;
    else if (!args[i].starts_with("--") && project.empty()) {
      const auto folder = f::absolute(args[i]).lexically_normal();
      if (f::exists(folder / "tsugumi.json")) project = folder.string();
    } else {
      Usage();
      return 64;
    }
  }
  if (!f::exists(launch.web_root / "index.html"))
    throw std::runtime_error("the Web UI is not built: " + launch.web_root.string());
  const auto ready = EnsureHost(launch);
  auto agent = ConnectAgent(ready, true);
  Tsugumi::Agent::ShowRequestT request;
  request.path = project;
  request.browser = browser;
  const auto shown = AgentCall<Tsugumi::Agent::Shown>(*agent, std::move(request));
  if (!shown.view->url() || !shown.view->url()->size())
    throw std::runtime_error("the host does not serve the Web UI");
  std::cout << "Tsugumi web UI: " << shown.view->url()->str() << '\n';
  if (shown.view->shown())
    std::cout << "A Tsugumi page shows it.\n";
  else if (shown.view->opened())
    std::cout << "The page opens in the default browser.\n";
  return 0;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  try {
    const auto executable = Executable(argv[0]);
    std::vector<std::string> args(argv + 1, argv + argc);
    const auto command = args.empty() ? "" : args[0];
    if (command == "--version") {
      const auto launch = LaunchFromEnvironment(executable);
      if (!launch.app.empty()) {
        std::cout << launch.version << '\n';
        return 0;
      }
      std::ifstream version(launch.root / "VERSION");
      std::cout << version.rdbuf();
      return 0;
    }
    if ((command == "update" || command == "uninstall") && !LaunchFromEnvironment(executable).app.empty())
      throw std::runtime_error("this command is part of " + LaunchFromEnvironment(executable).app.string() +
                               ": update or remove the app instead");
    if (command.empty() || command == "-h" || command == "--help") {
      Usage();
      PrintTools(executable);
      return 0;
    }
    if (command == "mcp") return RunMcp(executable);
    if (command == "update") return Update(LaunchFromEnvironment(executable).root);
    if (command == "uninstall") return Uninstall({args.begin() + 1, args.end()});
    if (command == "host-path") return HostPaths(executable, args);
    if (command == "web") return Open(executable, {args.begin() + 1, args.end()});
    if (command == "call") return RunCall(executable, {args.begin() + 1, args.end()});
    if (command == "lock" && args.size() > 2 && args[1] == "update" &&
        !args[2].starts_with("-"))
      args.insert(args.begin() + 2, "--name");
    if (const auto status = RunTool(executable, args)) return *status;
    Usage();
    return 64;
  } catch (const std::exception& error) {
    std::cerr << "tsugumi: " << error.what() << '\n';
    return 1;
  }
}
