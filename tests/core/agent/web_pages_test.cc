#include "host/web_pages.hh"

#include <gtest/gtest.h>

#include <cstdlib>
#include <optional>

#include "../agent_fixture_server.hh"

namespace tsugumi::test {
TEST(HostWebPagesTest, MissingBrowserLauncherReportsFailure) {
  TemporaryDirectory directory;
  Write(directory.path / "index.html", "");
  host::WebPages pages(nullptr, nullptr, {}, {}, {});
  pages.Serve(directory.path, 0, true, {});
  const auto* path = std::getenv("PATH");
  struct RestorePath {
    std::optional<std::string> path;
    ~RestorePath() {
      if (path)
        ::setenv("PATH", path->c_str(), 1);
      else
        ::unsetenv("PATH");
    }
  } restore{path ? std::optional<std::string>(path) : std::nullopt};
  ASSERT_EQ(::setenv("PATH", directory.path.c_str(), 1), 0);
  EXPECT_FALSE(pages.Show({}, {}, true).opened);
}

}  // namespace tsugumi::test
