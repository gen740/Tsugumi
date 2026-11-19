#include "host/binary_caches.hh"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <signal.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#ifdef __APPLE__
#include <crt_externs.h>
#endif
#include <filesystem>

#include "tsugumi/util/fs.hh"
#include "tsugumi/util/json_object.hh"
#include "tsugumi/util/process.hh"

namespace tsugumi::host {
namespace {
namespace j = tsugumi::json;
auto Cache(std::string url = "https://cache.example.org/team") -> j::Members {
  return {{"id", j::Quote("01234567-89ab-cdef-0123-456789abcdef")},
          {"name", j::Quote("Research")},
          {"url", j::Quote(url)},
          {"publicKey",
           j::Quote("research:BwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwc=")},
          {"username", "\"\""},
          {"password", "\"\""}};
}
auto List(const j::Members& cache) -> std::string {
  return j::Encode(std::vector<std::string>{j::Encode(cache)});
}
class BinaryCaches : public testing::Test {
 protected:
  void SetUp() override {
    std::string pattern =
        (std::filesystem::temp_directory_path() / "binary-caches-XXXXXX")
            .string();
    ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
    root = pattern;
  }
  void TearDown() override { std::filesystem::remove_all(root); }
  std::filesystem::path root;
};
TEST_F(BinaryCaches, RejectsInjectedSettingsAndCredentials) {
  for (const auto* url :
       {"file:///tmp/cache", "https://user:secret@example.org",
        "https://example.org?token=x", "https://example.org\ntrusted-users = *",
        "https://example.org/#fragment"})
    EXPECT_THROW(SaveBinaryCaches(root, List(Cache(url))), std::runtime_error);
  auto cache = Cache();
  cache["publicKey"] = j::Quote("research:not-a-key\ntrusted-users = *");
  EXPECT_THROW(SaveBinaryCaches(root, List(cache)), std::runtime_error);
  cache = Cache("http://cache.example.org/team");
  cache["username"] = j::Quote("token");
  cache["password"] = j::Quote("secret");
  EXPECT_THROW(SaveBinaryCaches(root, List(cache)), std::runtime_error);
  cache["url"] = j::Quote("https://cache.example.org/team");
  cache["password"] = j::Quote("secret\nmachine elsewhere password secret");
  EXPECT_THROW(SaveBinaryCaches(root, List(cache)), std::runtime_error);
  cache = Cache();
  EXPECT_THROW(SaveBinaryCaches(root, j::Encode(std::vector<std::string>{
                                          j::Encode(cache), j::Encode(cache)})),
               std::runtime_error);
}
TEST_F(BinaryCaches, StoresPrivateSettingsAndFreezesBootSnapshot) {
  auto cache = Cache();
  cache["username"] = j::Quote("token");
  cache["password"] = j::Quote("secret-for-test");
  const auto json = List(cache);
  SaveBinaryCaches(root, json);
  EXPECT_EQ(LoadBinaryCaches(root), json);
  const auto snapshot = PrepareBinaryCaches(root);
  EXPECT_TRUE(snapshot.problem.empty());
  EXPECT_NE(snapshot.configuration.find(
                "extra-substituters = https://cache.example.org/team"),
            std::string::npos);
  EXPECT_EQ(snapshot.configuration.find("secret-for-test"), std::string::npos);
  EXPECT_EQ(snapshot.netrc,
            "machine cache.example.org login token password secret-for-test\n");
  EXPECT_EQ(
      std::filesystem::status(root / "binary-caches.json").permissions() &
          std::filesystem::perms::mask,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
  SaveBinaryCaches(root, "[]");
  EXPECT_EQ(snapshot.netrc,
            "machine cache.example.org login token password secret-for-test\n");
  EXPECT_TRUE(PrepareBinaryCaches(root).configuration.empty());
  EXPECT_TRUE(PrepareBinaryCaches(root).netrc.empty());
}
TEST_F(BinaryCaches, InvalidSettingsApplyNothingAndNeverEchoCredentials) {
  auto good = Cache();
  good["username"] = j::Quote("token");
  good["password"] = j::Quote("secret-for-test");
  auto bad = Cache("https://other.example.org/team");
  bad["id"] = j::Quote("01234567-89ab-cdef-0123-456789abcdee");
  bad["publicKey"] = j::Quote("research:not-a-key");
  for (const auto& json :
       {j::Encode(std::vector<std::string>{j::Encode(good), j::Encode(bad)}),
        std::string("{\"secret-for-test\":"), std::string("{}")}) {
    fs::write_atomic(root / "binary-caches.json", json, 0600);
    EXPECT_THROW(LoadBinaryCaches(root), std::runtime_error);
    const auto snapshot = PrepareBinaryCaches(root);
    EXPECT_TRUE(snapshot.configuration.empty());
    EXPECT_TRUE(snapshot.netrc.empty());
    EXPECT_FALSE(snapshot.problem.empty());
    EXPECT_EQ(snapshot.problem.find("secret-for-test"), std::string::npos);
  }
}
}  // namespace
}  // namespace tsugumi::host

namespace tsugumi::host {
namespace {
TEST_F(BinaryCaches, OneHostHasOneCredential) {
  auto first = Cache("https://attic.example.org/one");
  first["username"] = j::Quote("token");
  first["password"] = j::Quote("first");
  auto second = Cache("https://attic.example.org/two");
  second["id"] = j::Quote("01234567-89ab-cdef-0123-456789abcdee");
  second["username"] = j::Quote("token");
  second["password"] = j::Quote("second");
  auto list = [&] {
    return j::Encode(
        std::vector<std::string>{j::Encode(first), j::Encode(second)});
  };
  EXPECT_THROW(SaveBinaryCaches(root, list()), std::runtime_error);
  second["password"] = first["password"];
  EXPECT_NO_THROW(SaveBinaryCaches(root, list()));
}
}  // namespace
}  // namespace tsugumi::host

#ifndef __APPLE__
extern char** environ;
#endif
namespace tsugumi::host {
namespace {
TEST_F(BinaryCaches, NixDownloadsFromAPrivateCacheWithTheHostSnapshot) {
  int input[2], output[2];
  ASSERT_EQ(::pipe(input), 0);
  ASSERT_EQ(::pipe(output), 0);
  for (const auto fd : {input[0], input[1], output[0], output[1]})
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
  const auto errors = root / "stderr";
  const auto stderr_fd =
      ::open(errors.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  ASSERT_GE(stderr_fd, 0);
  proc::Spec spec;
  spec.argv = {"/usr/bin/env", "python3", TSUGUMI_CACHE_PROBE, "--interactive"};
#ifdef __APPLE__
  auto** environment = *_NSGetEnviron();
#else
  auto** environment = ::environ;
#endif
  for (; *environment; ++environment) spec.env.emplace_back(*environment);
  spec.fds = {{0, input[0]}};
  spec.stdout_fd = output[1];
  spec.stderr_fd = stderr_fd;
  const auto child = proc::spawn(spec);
  ::close(input[0]);
  ::close(output[1]);
  ::close(stderr_fd);
  struct Process {
    pid_t pid;
    ~Process() {
      if (pid > 0) {
        proc::kill_group(pid, SIGTERM);
        proc::wait(pid);
      }
    }
  } process{child};
  std::string offered;
  char byte;
  while (::read(output[0], &byte, 1) == 1 && byte != '\n') offered += byte;
  ASSERT_FALSE(offered.empty()) << fs::read_file_or_throw(errors);
  auto cache = j::Object(offered);
  cache["id"] = Cache().at("id");
  cache["name"] = j::Quote("Probe");
  SaveBinaryCaches(root, List(cache));
  const auto snapshot = PrepareBinaryCaches(root);
  ASSERT_TRUE(snapshot.problem.empty()) << snapshot.problem;
  const auto directory = root / "snapshot";
  std::filesystem::create_directory(directory);
  fs::write_atomic(directory / "nix.conf", snapshot.configuration, 0600);
  fs::write_atomic(directory / "netrc", snapshot.netrc, 0600);
  const auto request = directory.string() + '\n';
  ASSERT_EQ(::write(input[1], request.data(), request.size()),
            static_cast<ssize_t>(request.size()));
  ::close(input[1]);
  while (::read(output[0], &byte, 1) == 1) {
  }
  ::close(output[0]);
  const auto status = proc::wait(child);
  process.pid = -1;
  EXPECT_TRUE(proc::exited_ok(status)) << fs::read_file_or_throw(errors);
}
}  // namespace
}  // namespace tsugumi::host
