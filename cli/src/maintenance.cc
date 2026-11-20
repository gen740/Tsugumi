#include "maintenance.hh"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <regex>
#include <sstream>

namespace tsugumi::cli {
namespace f = std::filesystem;
namespace {

auto ReadText(const f::path& file) -> std::string {
  std::ifstream in(file);
  std::stringstream s;
  s << in.rdbuf();
  auto text = s.str();
  while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
  return text;
}

auto Run(std::vector<std::string> argv, std::string* out = nullptr) -> int {
  int pipe_fds[2] = {-1, -1};
  if (out && ::pipe(pipe_fds) != 0) return 70;
  const pid_t pid = ::fork();
  if (pid < 0) return 70;
  if (pid == 0) {
    if (out) {
      ::dup2(pipe_fds[1], 1);
      ::close(pipe_fds[0]);
      ::close(pipe_fds[1]);
    }
    std::vector<char*> args;
    for (auto& a : argv) args.push_back(a.data());
    args.push_back(nullptr);
    ::execvp(args[0], args.data());
    _exit(127);
  }
  if (out) {
    ::close(pipe_fds[1]);
    std::array<char, 4096> buffer;
    ssize_t n;
    while ((n = ::read(pipe_fds[0], buffer.data(), buffer.size())) > 0) out->append(buffer.data(), static_cast<size_t>(n));
    ::close(pipe_fds[0]);
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : 70;
}

auto Stat(const f::path& p) -> std::optional<f::file_status> {
  std::error_code error;
  const auto status = f::symlink_status(p, error);
  if (error || status.type() == f::file_type::not_found) return std::nullopt;
  return status;
}

}  // namespace

// The latest release's install.sh (cli/install.sh) puts the newest version
// in place. TSUGUMI_INSTALL_BASE and TSUGUMI_VERSION go on to it as they are.
auto Update(const f::path& root) -> int {
  const auto* base_env = std::getenv("TSUGUMI_INSTALL_BASE");
  const std::string base = base_env ? base_env : "https://github.com/gen740/Tsugumi/releases/latest/download";
  const auto current = ReadText(root / "VERSION");
  if (!current.empty() && !base_env && !std::getenv("TSUGUMI_VERSION")) {
    // The latest release, from where GitHub sends releases/latest (no
    // download when this is it).
    std::string redirect;
    Run({"curl", "-fsSI", "-o", "/dev/null", "-w", "%{redirect_url}", "https://github.com/gen740/Tsugumi/releases/latest"},
        &redirect);
    std::smatch m;
    if (std::regex_search(redirect, m, std::regex(R"(/tag/v([^/\s]+)\s*$)")) && m[1] == current) {
      std::cout << "tsugumi " << current << " is up to date.\n";
      return 0;
    }
  }
  const auto status = Run({"/bin/sh", "-c",
                           "script=\"$(curl -fsSL \"$1/install.sh\")\" || { echo \"Could not download $1/install.sh.\" >&2; exit 1; }\n"
                           "printf '%s\\n' \"$script\" | /bin/sh",
                           "sh", base});
  if (status == 0) std::cout << "A Tsugumi host that runs keeps the old version until it stops.\n";
  return status == 1 ? 69 : status;
}

// Remove only files owned by cli/install.sh; user data and Nix stay in place.
auto Uninstall(const std::vector<std::string>& args) -> int {
  try {
    if (args.size() > 1 || (args.size() == 1 && args[0] != "--yes")) {
      std::cerr << "Usage: tsugumi uninstall [--yes]\n";
      return 64;
    }
    const bool yes = args.size() == 1;
    const auto* home_env = std::getenv("HOME");
    if (!home_env) throw std::runtime_error("HOME is not set");
    const f::path home = home_env;
    const auto local = home / ".local";
    const auto share = local / "share";
    const auto bin = local / "bin";
    const auto data = share / "tsugumi";
    const auto command = bin / "tsugumi";
    const auto current = data / "current";
    // Never follow a redirected install root or launcher folder when deleting.
    for (const auto& dir : {local, share, bin, data}) {
      const auto s = Stat(dir);
      if (s && s->type() != f::file_type::directory)
        throw std::runtime_error("Refusing to remove a redirected installation: " + dir.string());
    }
    const auto launcher = Stat(command);
    if (launcher && (launcher->type() != f::file_type::symlink ||
                     (bin / f::read_symlink(command)).lexically_normal() != current / "tsugumi"))
      throw std::runtime_error("Not an installer-owned launcher: " + command.string());
    auto owned = [&](const f::path& release, const std::string& version) {
      const auto r = Stat(release);
      const auto m = Stat(release / ".installed-by-tsugumi");
      return r && r->type() == f::file_type::directory && m && m->type() == f::file_type::regular &&
             ReadText(release / ".installed-by-tsugumi") == version;
    };
    std::vector<f::path> targets;
    if (const auto link = Stat(current)) {
      if (link->type() != f::file_type::symlink)
        throw std::runtime_error("Not an installer-owned version link: " + current.string());
      const auto version = f::read_symlink(current).string();
      if (!std::regex_match(version, std::regex(R"([A-Za-z0-9][A-Za-z0-9._+-]*)")))
        throw std::runtime_error("Invalid installed version: " + version);
      if (!owned(data / version, version))
        throw std::runtime_error("Not an installer-owned release: " + (data / version).string());
      targets.push_back(data / version);
      targets.push_back(current);
    }
    // Old immutable generations stay available during updates. The user stops
    // running hosts before explicitly uninstalling them; foreign entries stay.
    if (Stat(data))
      for (const auto& entry : f::directory_iterator(data)) {
        const auto name = entry.path().filename().string();
        if (std::find(targets.begin(), targets.end(), entry.path()) == targets.end() && owned(entry.path(), name))
          targets.push_back(entry.path());
      }
    if (launcher) targets.push_back(command);
    if (targets.empty()) {
      std::cout << "No installer-managed tsugumi command is installed.\n";
      return 0;
    }
    std::cout << "Remove the tsugumi command and its installed runtime:\n";
    for (const auto& target : targets) std::cout << "  " << target.string() << '\n';
    std::cout << "Projects, settings, the shared Mac VM, the desktop app, Nix and its store are kept.\n";
    if (!yes) {
      std::cout << "Quit Tsugumi and stop the Tsugumi host, then run: tsugumi uninstall --yes\n";
      return 0;
    }
    for (const auto& target : targets) f::remove_all(target);
    // Extra files here do not belong to this installation and must survive.
    if (Stat(data) && f::is_empty(data)) f::remove(data);
    std::cout << "The tsugumi command has been uninstalled.\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "tsugumi: " << error.what() << '\n';
    return 1;
  }
}

}  // namespace tsugumi::cli
