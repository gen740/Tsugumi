#include "host/web_pages.hh"

#include <fcntl.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <optional>
#include <thread>

#include "../agent_fixture_server.hh"
#include "host/descriptor.hh"

namespace tsugumi::test {
namespace {
using namespace std::chrono_literals;

struct RestorePath {
  std::optional<std::string> path;

  RestorePath() {
    if (const auto* value = std::getenv("PATH")) path = value;
  }

  ~RestorePath() {
    if (path)
      ::setenv("PATH", path->c_str(), 1);
    else
      ::unsetenv("PATH");
  }
};

TEST(HostWebPagesTest, MissingBrowserLauncherReportsFailure) {
  TemporaryDirectory directory;
  Write(directory.path / "index.html", "");
  host::WebPages pages(nullptr, nullptr, {}, {}, {});
  pages.Serve(directory.path, 0, true, {});
  const RestorePath restore;
  ASSERT_EQ(::setenv("PATH", directory.path.c_str(), 1), 0);
  EXPECT_FALSE(pages.Show({}, {}, true).opened);
}

TEST(HostWebPagesTest, BrowserLauncherDoesNotInheritHostDescriptors) {
  TemporaryDirectory directory;
  Write(directory.path / "index.html", "");
  const host::Descriptor marker(
      ::open((directory.path / "index.html").c_str(), O_RDONLY | O_CLOEXEC));
  ASSERT_GE(marker.get(), 0);
  const host::Descriptor inherited(::fcntl(marker.get(), F_DUPFD, 100));
  ASSERT_GE(inherited.get(), 100);
  const auto script =
      "#!/bin/sh\n"
      "if [ -e /dev/fd/" +
      std::to_string(inherited.get()) +
      " ]; then\n"
      "  : > \"${0%/*}/inherited\"\n"
      "else\n"
      "  : > \"${0%/*}/closed\"\n"
      "fi\n";
  for (const auto* name : {"open", "xdg-open", "rundll32.exe"}) {
    const auto launcher = directory.path / name;
    Write(launcher, script);
    std::filesystem::permissions(launcher, std::filesystem::perms::owner_all);
  }
  const RestorePath restore;
  ASSERT_EQ(::setenv("PATH", directory.path.c_str(), 1), 0);
  host::WebPages pages(nullptr, nullptr, {}, {}, {});
  pages.Serve(directory.path, 0, true, {});
  ASSERT_TRUE(pages.OpenBrowser());
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!std::filesystem::exists(directory.path / "closed") &&
         !std::filesystem::exists(directory.path / "inherited") &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(10ms);
  EXPECT_TRUE(std::filesystem::exists(directory.path / "closed"));
  EXPECT_FALSE(std::filesystem::exists(directory.path / "inherited"));
}

}  // namespace
}  // namespace tsugumi::test
