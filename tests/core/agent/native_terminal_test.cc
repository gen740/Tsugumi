#include "host/native_terminal.hh"

#include <gtest/gtest.h>

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
  const std::string old_home = std::getenv("HOME") ? std::getenv("HOME") : "";
  ::setenv("HOME", directory.path.c_str(), 1);
  ::setenv("TSUGUMI_TERMINAL_TEST_SECRET", "MUST_NOT_CROSS", 1);
  H::HostTerminalRequestT request;
  request.project = directory.path.string();
  H::HostTerminalT opened;
  try {
    opened = service.Handle(request);
  } catch (...) {
    ::setenv("HOME", old_home.c_str(), 1);
    ::unsetenv("TSUGUMI_TERMINAL_TEST_SECRET");
    throw;
  }
  ::setenv("HOME", old_home.c_str(), 1);
  ::unsetenv("TSUGUMI_TERMINAL_TEST_SECRET");
  request.session = opened.session;
  ASSERT_GT(request.session, 0u);
  request.action = H::TerminalAction::Resize;
  request.rows = 31;
  request.cols = 91;
  EXPECT_NO_THROW(service.Handle(request));
  request.action = H::TerminalAction::Input;
  const std::string command =
      "unset HISTFILE; printf 'secret=%s\\n' "
      "\"${TSUGUMI_TERMINAL_TEST_SECRET-unset}\"; pwd; stty size; exit\n";
  request.data.assign(command.begin(), command.end());
  service.Handle(request);
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(5),
                                 [&] { return closed; }));
    EXPECT_NE(output.find("secret=unset"), output.npos);
    EXPECT_EQ(output.find("MUST_NOT_CROSS"), output.npos);
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
