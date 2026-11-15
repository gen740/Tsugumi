#include "host/native_terminal.hh"

#include <gtest/gtest.h>
#include <pwd.h>
#include <unistd.h>

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
TEST(NativeTerminalTest, HostOwnsPtyInputResizeCloseAndStripsSecrets) {
  TemporaryDirectory directory;
  std::mutex mutex;
  std::condition_variable changed;
  std::string output;
  bool closed = false;
  host::NativeTerminals service([&](const H::HostTerminalDataT& event) {
    std::lock_guard lock(mutex);
    output.append(event.data.begin(), event.data.end());
    closed |= event.closed;
    changed.notify_all();
  });
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
  passwd user{};
  passwd* found = nullptr;
  std::vector<char> buffer(16384);
  ASSERT_EQ(
      ::getpwuid_r(::getuid(), &user, buffer.data(), buffer.size(), &found), 0);
  ASSERT_NE(found, nullptr);
  const std::string home = user.pw_dir;
  const std::string username = user.pw_name;
  // Exercise spaces, '=' in values and the /tmp alias used on macOS.
  const auto project = directory.path / "terminal project=1";
  std::filesystem::create_directory(project);
  H::HostTerminalRequestT request;
  request.project = project.string();
  const auto opened = service.Handle(request);
  request.session = opened.session;
  ASSERT_GT(request.session, 0u);
  request.action = H::TerminalAction::Resize;
  request.rows = 31;
  request.cols = 23;
  EXPECT_NO_THROW(service.Handle(request));
  request.action = H::TerminalAction::Input;
  const std::string command =
      "unset HISTFILE; printf '\\0BEGIN\\0'; "
      "for name in $(compgen -e); do "
      "printf '%s=%s\\0' \"$name\" \"${!name}\"; done; "
      "printf 'size=%s\\0' \"$(stty size)\"; printf '\\0END\\0'; exit\n";
  request.data.assign(command.begin(), command.end());
  service.Handle(request);
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(5),
                                 [&] { return closed; }));
    EXPECT_EQ(output.find("MUST_NOT_CROSS"), output.npos);
    // Actual NUL bytes distinguish command output from echoed/readline-wrapped
    // input and prompts, even in a narrow PTY. Split only at the first '='.
    const auto begin = output.find(std::string("\0BEGIN\0", 7));
    ASSERT_NE(begin, output.npos);
    const auto end = output.find(std::string("\0END\0", 5), begin + 7);
    ASSERT_NE(end, output.npos);
    std::istringstream records(output.substr(begin + 7, end - begin - 7));
    std::map<std::string, std::string> actual;
    std::string record;
    while (std::getline(records, record, '\0')) {
      const auto separator = record.find('=');
      ASSERT_NE(separator, record.npos) << record;
      const auto inserted = actual
                                .emplace(record.substr(0, separator),
                                         record.substr(separator + 1))
                                .second;
      EXPECT_TRUE(inserted) << record;
    }
    std::map<std::string, std::string> expected = {
        {"HOME", home},
        {"USER", username},
        {"LOGNAME", username},
        {"SHELL", user.pw_shell[0] ? user.pw_shell : "/bin/bash"},
        {"TMPDIR", "/tmp"},
        {"LANG", "C.UTF-8"},
        {"PATH",
         home + "/.nix-profile/bin:/nix/var/nix/profiles/default/bin:"
                "/run/current-system/sw/bin:/usr/bin:/bin:/usr/sbin:/sbin"},
        {"TERM", "xterm-256color"},
        {"COLORTERM", "truecolor"},
        {"TSUGUMI_PROJECT", project.string()},
        // Bash itself exports these; they are not inherited from agentd.
        {"PWD", project.string()},
        {"SHLVL", "1"},
        {"size", "31 23"}};
    for (const auto* name : {"HOME", "TMPDIR", "TSUGUMI_PROJECT", "PWD"}) {
      ASSERT_TRUE(actual.contains(name)) << name;
      actual.at(name) = std::filesystem::canonical(actual.at(name)).string();
      expected.at(name) =
          std::filesystem::canonical(expected.at(name)).string();
    }
    EXPECT_EQ(actual, expected);
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
