// The tsugumi command: opens the Web UI, which hostd serves. The host keeps
// running after the command ends, like the desktop app's.
//
//   tsugumi [--port N] [--no-browser] [project folder]
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

#include "launch.hh"
#include "maintenance.hh"
#include "mcp.hh"

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
  std::cout << "Usage: tsugumi [--port N] [--no-browser] [project folder]\n"
               "       tsugumi mcp | update | uninstall [--yes] | --version\n";
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
  auto host = ConnectHost(ready);
  const auto web = ServeWeb(*host, launch, browser, project);
  std::cout << "Tsugumi web UI: " << web.page_url << '\n';
  std::cout << "  host:  " << launch.state.string() << " (PID " << ready.pid << ")\n";
  if (browser) std::cout << "The page opens in the default browser. The host keeps running after this command.\n";
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
    if (command == "-h" || command == "--help") {
      Usage();
      return 0;
    }
    if (command == "mcp") return RunMcp(executable);
    if (command == "update") return Update(LaunchFromEnvironment(executable).root);
    if (command == "uninstall") return Uninstall({args.begin() + 1, args.end()});
    return Open(executable, args);
  } catch (const std::exception& error) {
    std::cerr << "tsugumi: " << error.what() << '\n';
    return 1;
  }
}
