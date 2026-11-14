#include "host/native_terminal.hh"

#include <gtest/gtest.h>
#include <pwd.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>

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
  const auto* user = ::getpwuid(::getuid());
  ASSERT_NE(user, nullptr);
  const std::string home = user->pw_dir;
  const std::string username = user->pw_name;
  H::HostTerminalRequestT request;
  request.project = directory.path.string();
  const auto opened = service.Handle(request);
  request.session = opened.session;
  ASSERT_GT(request.session, 0u);
  request.action = H::TerminalAction::Resize;
  request.rows = 31;
  request.cols = 91;
  EXPECT_NO_THROW(service.Handle(request));
  request.action = H::TerminalAction::Input;
  const std::string command =
      "unset HISTFILE; printf 'secret=%s,%s,%s,%s,%s,%s,%s\\n' "
      "\"${OPENAI_API_KEY-unset}\" \"${ANTHROPIC_API_KEY-unset}\" "
      "\"${FUTURE_SECRET-unset}\" \"${BASH_ENV-unset}\" \"${ENV-unset}\" "
      "\"${NODE_OPTIONS-unset}\" "
      "\"${TSUGUMI_EXECUTION_BROKER_CAPABILITY_FILE-unset}\"; "
      "printf 'home=%s user=%s logname=%s tmp=%s lang=%s project=%s\\n' "
      "\"$HOME\" \"$USER\" \"$LOGNAME\" \"$TMPDIR\" \"$LANG\" "
      "\"$TSUGUMI_PROJECT\"; pwd; stty size; exit\n";
  request.data.assign(command.begin(), command.end());
  service.Handle(request);
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(5),
                                 [&] { return closed; }));
    EXPECT_NE(output.find("secret=unset,unset,unset,unset,unset,unset,unset"),
              output.npos);
    EXPECT_EQ(output.find("MUST_NOT_CROSS"), output.npos);
    EXPECT_NE(output.find("home=" + home + " user=" + username + " logname=" +
                          username + " tmp=/tmp lang=C.UTF-8 project=" +
                          directory.path.string()),
              output.npos);
    EXPECT_NE(output.find(directory.path.string()), output.npos);
    EXPECT_NE(output.find("31 91"), output.npos);
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
