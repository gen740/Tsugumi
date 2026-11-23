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
               "       tsugumi call <RequestName> ['<json>']\n"
               "       tsugumi web [--port N] [--no-browser] [project folder]\n"
               "       tsugumi mcp\n"
               "       tsugumi update | uninstall [--yes] | --version\n"
               "Tools work as the host's Agent principal, with the same access "
               "as Claude Code's.\n";
}

auto Open(const f::path& executable, const std::vector<std::string>& args) -> int {
  auto launch = LaunchFromEnvironment(executable);
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
      std::ifstream version(LaunchFromEnvironment(executable).root / "VERSION");
      std::cout << version.rdbuf();
      return 0;
    }
    if (command.empty() || command == "-h" || command == "--help") {
      Usage();
      PrintTools(executable);
      return 0;
    }
    if (command == "mcp") return RunMcp(executable);
    if (command == "update") return Update(LaunchFromEnvironment(executable).root);
    if (command == "uninstall") return Uninstall({args.begin() + 1, args.end()});
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
