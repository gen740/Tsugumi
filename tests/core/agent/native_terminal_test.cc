#include "host/native_terminal.hh"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <sstream>

#include "../agent_fixture_server.hh"

namespace tsugumi::test {
namespace H = Tsugumi::Host;
TEST(NativeTerminalTest, HostOwnsPtyInputCloseAndStripsSecrets) {
  TemporaryDirectory directory;
  const auto home = directory.path / "empty home";
  std::filesystem::create_directory(home);
  // Readiness is emitted by Bash after its login startup, before accepting
  // input.
  Write(home / ".bash_profile", "printf '\\0READY\\0'\n");
  // A Nix build sandbox has no /bin/bash: use the one on PATH there.
  std::string bash = "/bin/bash";
  if (!std::filesystem::exists(bash)) {
    std::istringstream path(std::getenv("PATH") ? std::getenv("PATH") : "");
    for (std::string entry; std::getline(path, entry, ':');)
      if (std::filesystem::exists(std::filesystem::path(entry) / "bash")) {
        bash = (std::filesystem::path(entry) / "bash").string();
        break;
      }
  }
  const auto shell = directory.path / "login-shell";
  // Capture the environment at the exec boundary, before system profiles can
  // rewrite it. This account shell then starts a normal Bash login session.
  Write(shell, "#!" + bash +
                   "\n"
                   "printf '\\0PASSED\\0'\n"
                   "for name in $(compgen -e); do "
                   "printf '%s=%s\\0' \"$name\" \"${!name}\"; done\n"
                   "printf '\\0END\\0'\n"
                   "exec " +
                   bash + " \"$@\"\n");
  std::filesystem::permissions(shell, std::filesystem::perms::owner_all);
  const host::NativeTerminals::Account account{home.string(), "terminal-user",
                                               shell.string()};
  std::mutex mutex;
  std::condition_variable changed;
  std::string output;
  bool closed = false;
  host::NativeTerminals service(
      [&](const H::HostTerminalDataT& event) {
        std::lock_guard lock(mutex);
        output.append(event.data.begin(), event.data.end());
        closed |= event.closed;
        changed.notify_all();
      },
      [account] { return account; });
  const std::vector<std::string> names = {
      "HOME",
      "USER",
      "LOGNAME",
      "SHELL",
      "PATH",
      "TMPDIR",
      "LANG",
      "TERM",
      "COLORTERM",
      "TSUGUMI_PROJECT",
      "OPENAI_API_KEY",
      "ANTHROPIC_API_KEY",
      "FUTURE_SECRET",
      "BASH_ENV",
      "ENV",
      "NODE_OPTIONS",
      "TSUGUMI_EXECUTION_BROKER_CAPABILITY_FILE"};
  std::vector<std::optional<std::string>> previous;
  for (const auto& name : names) {
    const auto* value = std::getenv(name.c_str());
    previous.push_back(value ? std::optional<std::string>(value)
                             : std::nullopt);
    ASSERT_EQ(::setenv(name.c_str(), "MUST_NOT_CROSS", 1), 0);
  }
  struct Restore {
    const std::vector<std::string>& names;
    const std::vector<std::optional<std::string>>& previous;
    ~Restore() {
      for (size_t i = 0; i < names.size(); ++i) {
        if (previous[i])
          ::setenv(names[i].c_str(), previous[i]->c_str(), 1);
        else
          ::unsetenv(names[i].c_str());
      }
    }
  } restore{names, previous};
  // Exercise spaces, '=' in values and the /tmp alias used on macOS.
  const auto project = directory.path / "terminal project=1";
  std::filesystem::create_directory(project);
  H::HostTerminalRequestT request;
  request.project = project.string();
  const auto opened = service.Handle(request);
  request.session = opened.session;
  ASSERT_GT(request.session, 0u);
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(30), [&] {
      return output.find(std::string("\0READY\0", 7)) != output.npos || closed;
    }));
    ASSERT_FALSE(closed);
  }
  request.action = H::TerminalAction::Input;
  const std::string command =
      "unset HISTFILE; printf '\\0BEGIN\\0'; "
      "for name in $(compgen -e); do "
      "printf '%s=%s\\0' \"$name\" \"${!name}\"; done; "
      "shopt -q login_shell; printf 'login=%s\\0' \"$?\"; "
      "printf '\\0END\\0'; exit\n";
  request.data.assign(command.begin(), command.end());
  service.Handle(request);
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(30),
                                 [&] { return closed; }));
    EXPECT_EQ(output.find("MUST_NOT_CROSS"), output.npos);
    // Actual NUL bytes distinguish command output from echoed/readline-wrapped
    // input and prompts, even in a narrow PTY. Split only at the first '='.
    const auto read_environment = [&](const std::string& marker) {
      std::map<std::string, std::string> result;
      const auto begin = output.find(marker);
      EXPECT_NE(begin, output.npos);
      if (begin == output.npos) return result;
      const auto end =
          output.find(std::string("\0END\0", 5), begin + marker.size());
      EXPECT_NE(end, output.npos);
      if (end == output.npos) return result;
      std::istringstream records(
          output.substr(begin + marker.size(), end - begin - marker.size()));
      std::string record;
      while (std::getline(records, record, '\0')) {
        const auto separator = record.find('=');
        EXPECT_NE(separator, record.npos) << record;
        if (separator == record.npos) continue;
        EXPECT_TRUE(result
                        .emplace(record.substr(0, separator),
                                 record.substr(separator + 1))
                        .second)
            << record;
      }
      return result;
    };
    auto passed = read_environment(std::string("\0PASSED\0", 8));
    auto actual = read_environment(std::string("\0BEGIN\0", 7));
    std::map<std::string, std::string> expected = {
        {"HOME", home.string()},
        {"USER", account.name},
        {"LOGNAME", account.name},
        {"SHELL", shell.string()},
        {"TMPDIR", "/tmp"},
        {"LANG", "C.UTF-8"},
        {"TERM", "xterm-256color"},
        {"COLORTERM", "truecolor"},
        {"TSUGUMI_PROJECT", project.string()},
        {"PWD", project.string()}};
    for (const auto* name :
         {"HOME", "SHELL", "TMPDIR", "TSUGUMI_PROJECT", "PWD"}) {
      ASSERT_TRUE(passed.contains(name)) << name;
      passed.at(name) = std::filesystem::canonical(passed.at(name)).string();
      expected.at(name) =
          std::filesystem::canonical(expected.at(name)).string();
    }
    for (const auto& [name, value] : expected) {
      ASSERT_TRUE(passed.contains(name)) << name;
      EXPECT_EQ(passed.at(name), value) << name;
    }
    // Check the host's PATH in order; extra entries are allowed. Profiles may
    // extend or reorder PATH later (for example macOS path_helper).
    ASSERT_TRUE(passed.contains("PATH"));
    std::istringstream paths(passed.at("PATH"));
    const std::vector<std::string> expected_paths = {
        (home / ".nix-profile/bin").string(),
        "/nix/var/nix/profiles/default/bin",
        "/run/current-system/sw/bin",
        "/usr/bin",
        "/bin",
        "/usr/sbin",
        "/sbin"};
    auto next = expected_paths.begin();
    std::string path;
    while (std::getline(paths, path, ':') && next != expected_paths.end()) {
      if (std::filesystem::weakly_canonical(path) ==
          std::filesystem::weakly_canonical(*next))
        ++next;
    }
    EXPECT_EQ(next, expected_paths.end());
    // System profiles may export more variables. Only the poisoned host values
    // must be absent, both at exec and after normal login startup files ran.
    for (const auto* environment : {&passed, &actual}) {
      for (const auto& [name, value] : *environment)
        EXPECT_EQ(value.find("MUST_NOT_CROSS"), value.npos) << name;
    }
    ASSERT_TRUE(actual.contains("login"));
    EXPECT_EQ(actual.at("login"), "0");
  }
  request.action = H::TerminalAction::Close;
  EXPECT_NO_THROW(service.Handle(request));
  EXPECT_NO_THROW(service.Handle(request));
  service.Shutdown();
}
TEST(NativeTerminalTest, InvalidSessionAndSizeDoNotLaunchShells) {
  host::NativeTerminals service({});
  H::HostTerminalRequestT request;
  request.cols = 1000;
  EXPECT_THROW(service.Handle(request), std::runtime_error);
  request.action = H::TerminalAction::Input;
  request.session = 999;
  EXPECT_THROW(service.Handle(request), std::runtime_error);
  request.action = H::TerminalAction::Close;
  EXPECT_NO_THROW(service.Handle(request));
}
}  // namespace tsugumi::test
