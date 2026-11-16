#include "host/format.hh"

#include <gtest/gtest.h>

#include <cstdlib>

#include "host/projects.hh"
#include "util/crypto.hh"
#include "util/fs.hh"

namespace tsugumi::host {
namespace {
TEST(HostFormat, JsonAndDiagnosticsNeverReadOrWriteAProject) {
  Tsugumi::Host::FileFormatRequestT request;
  request.path = "definitions/missing/_card.json";
  request.text = R"({"a":[1,2],"b":{}})";
  const auto result = Format(request);
  EXPECT_EQ(result.text, "{\n  \"a\": [\n    1,\n    2\n  ],\n  \"b\": {}\n}\n");
  EXPECT_TRUE(result.diagnostics.empty());
  request.text = result.text;
  EXPECT_EQ(Format(request).text, result.text);
  request.text = "{\"a\":";
  const auto bad = Format(request);
  ASSERT_EQ(bad.diagnostics.size(), 1u);
  EXPECT_EQ(bad.text, request.text);
  EXPECT_EQ(bad.diagnostics.front()->code, "syntax_error");
  EXPECT_EQ(bad.diagnostics.front()->path, request.path);
  request.path = "x.fbs";
  EXPECT_THROW(Format(request), ProjectFailure);
  request.path = "../outside.json";
  EXPECT_THROW(Format(request), ProjectFailure);
}
TEST(HostFormat, PinnedToolsFormatRepeatablyAndKeepInvalidInput) {
  Tsugumi::Host::FileFormatRequestT request;
  request.path = "x.py";
  request.text = "def f(:\n  pass\n";
  Tsugumi::Host::FileFormatResultT bad;
  try {
    bad = Format(request);
  } catch (const ProjectFailure& error) {
    if (error.code == "unsupported") GTEST_SKIP() << "Host formatter paths were not configured";
    throw;
  }
  ASSERT_EQ(bad.diagnostics.size(), 1u);
  EXPECT_EQ(bad.text, request.text);
  EXPECT_TRUE(bad.diagnostics.front()->offset.has_value());
  EXPECT_EQ(bad.diagnostics.front()->message.find("tsugumi-format-"), std::string::npos);
  request.text = "x= [1,2]\n";
  const auto first = Format(request);
  request.text = first.text;
  EXPECT_EQ(Format(request).text, first.text);
  EXPECT_TRUE(first.diagnostics.empty());
}
TEST(HostFormat, FixedToolsIgnorePathAndUserConfiguration) {
  const auto dir = std::filesystem::temp_directory_path() / ("formatter-environment-" + crypto::uuid4());
  fs::write_atomic(dir / "ruff", "#!/bin/sh\necho WRONG\n", 0700);
  fs::write_atomic(dir / "ruff.toml", "line-length = 1\n", 0600);
  const auto old_path = std::getenv("PATH") ? std::optional<std::string>(std::getenv("PATH")) : std::nullopt;
  const auto old_home = std::getenv("HOME") ? std::optional<std::string>(std::getenv("HOME")) : std::nullopt;
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
  for (const auto& [name, text] :
       std::vector<std::pair<std::string, std::string>>{{"x.py", "x=[1,2]\n"},
                                                        {"x.cc", "int  main( ){return 0;}"},
                                                        {"x.nix", "{ a = 1; b = [ 1 2 ]; }"},
                                                        {"x.sh", "if true;then echo hi;fi\n"}}) {
    SCOPED_TRACE(name);
    Tsugumi::Host::FileFormatRequestT request;
    request.path = name;
    request.text = text;
    Tsugumi::Host::FileFormatResultT first;
    try {
      first = Format(request);
    } catch (const ProjectFailure& error) {
      if (error.code == "unsupported") GTEST_SKIP() << "Host formatter paths were not configured";
      throw;
    }
    EXPECT_TRUE(first.diagnostics.empty());
    EXPECT_NE(first.text, text);
    EXPECT_EQ(first.text.find("WRONG"), std::string::npos);
    request.text = first.text;
    EXPECT_EQ(Format(request).text, first.text);
  }
}
}  // namespace
}  // namespace tsugumi::host
