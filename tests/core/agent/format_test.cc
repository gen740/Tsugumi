#include "host/format.hh"

#include <gtest/gtest.h>

#include <cstdlib>

#include "host/projects.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/util/json_object.hh"

namespace tsugumi::host {
namespace {
TEST(HostFormat, JsonAndDiagnosticsNeverReadOrWriteAProject) {
  Tsugumi::Host::FileFormatRequestT request;
  request.path = "definitions/missing/_card.json";
  request.text = R"({"a":[1,2],"b":{}})";
  const auto result = Format(request);
  EXPECT_EQ(json::Array(json::Get(json::Object(result.text), "a")),
            (std::vector<std::string>{"1", "2"}));
  EXPECT_TRUE(json::Object(json::Get(json::Object(result.text), "b")).empty());
  EXPECT_TRUE(result.diagnostics.empty());
  request.text = result.text;
  EXPECT_EQ(Format(request).text, result.text);
  request.text = "{\"a\":";
  const auto bad = Format(request);
  ASSERT_FALSE(bad.diagnostics.empty());
  EXPECT_EQ(bad.text, request.text);
  EXPECT_EQ(bad.diagnostics.front()->code, "syntax_error");
  EXPECT_EQ(bad.diagnostics.front()->path, request.path);
  request.path = "../outside.json";
  EXPECT_THROW(Format(request), ProjectFailure);
}
TEST(HostFormat, OtherExtensionsAreUnsupported) {
  for (const auto* path : {"x.fbs", "x.cc", "x.py", "x.sh", "README"}) {
    SCOPED_TRACE(path);
    Tsugumi::Host::FileFormatRequestT request;
    request.path = path;
    request.text = "{}";
    try {
      Format(request);
      ADD_FAILURE() << "The extension was accepted";
    } catch (const ProjectFailure& error) {
      EXPECT_EQ(error.code, "unsupported");
    }
  }
}
TEST(HostFormat, InvalidNixKeepsTheInputAndReportsAnOffset) {
  Tsugumi::Host::FileFormatRequestT request;
  request.path = "definitions/test/default.nix";
  request.text = "{ a = ; }\n";
  Tsugumi::Host::FileFormatResultT bad;
  try {
    bad = Format(request);
  } catch (const ProjectFailure& error) {
    if (error.code == "unsupported")
      GTEST_SKIP() << "nixfmt was not configured";
    throw;
  }
  ASSERT_FALSE(bad.diagnostics.empty());
  EXPECT_EQ(bad.text, request.text);
  EXPECT_EQ(bad.diagnostics.front()->offset, 6u);
  EXPECT_EQ(bad.diagnostics.front()->path, request.path);
  EXPECT_NE(bad.diagnostics.front()->message.find(request.path),
            std::string::npos);
  EXPECT_EQ(bad.diagnostics.front()->message.find("tsugumi-format-"),
            std::string::npos);
}
TEST(HostFormat, NixFormattingUsesThePinnedExecutable) {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("formatter-environment-" + crypto::uuid4());
  fs::write_atomic(dir / "nixfmt", "#!/bin/sh\necho WRONG\n", 0700);
  const auto old_path = std::getenv("PATH")
                            ? std::optional<std::string>(std::getenv("PATH"))
                            : std::nullopt;
  const auto old_home = std::getenv("HOME")
                            ? std::optional<std::string>(std::getenv("HOME"))
                            : std::nullopt;
  struct Restore {
    std::filesystem::path dir;
    std::optional<std::string> path, home;
    ~Restore() {
      if (path)
        setenv("PATH", path->c_str(), 1);
      else
        unsetenv("PATH");
      if (home)
        setenv("HOME", home->c_str(), 1);
      else
        unsetenv("HOME");
      std::filesystem::remove_all(dir);
    }
  } restore{dir, old_path, old_home};
  setenv("PATH", dir.c_str(), 1);
  setenv("HOME", dir.c_str(), 1);
  Tsugumi::Host::FileFormatRequestT request;
  request.path = "missing/default.nix";
  request.text = "{ a = 1; b = [ 1 2 ]; }";
  Tsugumi::Host::FileFormatResultT first;
  try {
    first = Format(request);
  } catch (const ProjectFailure& error) {
    if (error.code == "unsupported")
      GTEST_SKIP() << "nixfmt was not configured";
    throw;
  }
  EXPECT_TRUE(first.diagnostics.empty());
  EXPECT_NE(first.text, request.text);
  EXPECT_EQ(first.text.find("WRONG"), std::string::npos);
  request.text = first.text;
  EXPECT_EQ(Format(request).text, first.text);
}
}  // namespace
}  // namespace tsugumi::host
