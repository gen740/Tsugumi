// The shipped JSON-lines CLI talks to real core listeners in these scenarios.
#include <fcntl.h>
#include <gtest/gtest.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "agent_fixture_server.hh"
#include "test_document.hh"
#include "tsugumi/rpc/unix_socket.hh"

extern char** environ;

namespace tsugumi::test {
namespace {
using namespace std::chrono_literals;

struct CliResult {
  int status;
  std::string error;
  std::vector<std::string> replies;
};

auto Request(uint64_t id, std::string_view type, std::string_view value)
    -> std::string {
  return "{\"payload\":{\"value\":" + std::string(value) +
         ",\"value_type\":" + Quote(type) +
         "},\"request_id\":" + std::to_string(id) +
         ",\"payload_type\":\"Request\"}\n";
}

class RpcCli : public ::testing::TestWithParam<bool> {
 protected:
  auto Endpoint(const std::string& server) const -> std::string {
    return GetParam() ? JsonString(server, "/endpoint")
                      : "unix:" + JsonString(server, "/unix_socket");
  }

  auto Run(const std::string& input, const std::string& endpoint,
           const std::string& schema) -> CliResult {
    const auto stderr_path = directory.path / "cli.error";
    int input_pipe[2], output_pipe[2];
    if (::pipe(input_pipe)) throw std::runtime_error("cannot create CLI input");
    if (::pipe(output_pipe)) {
      ::close(input_pipe[0]);
      ::close(input_pipe[1]);
      throw std::runtime_error("cannot create CLI output");
    }
    const std::vector<std::string> arguments{
        TSUGUMI_RPC_BINARY, "--endpoint", endpoint, "--schema", schema,
        "--timeout-ms",     "3000"};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, input_pipe[0], STDIN_FILENO);
    posix_spawn_file_actions_addclose(&actions, input_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, input_pipe[1]);
    posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, output_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, output_pipe[1]);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO,
                                     stderr_path.c_str(),
                                     O_CREAT | O_WRONLY | O_TRUNC, 0600);
    std::vector<char*> argv;
    for (const auto& argument : arguments)
      argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    pid_t pid = -1;
    const auto result = posix_spawn(&pid, argv.front(), &actions, nullptr,
                                    argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(input_pipe[0]);
    ::close(output_pipe[1]);
    if (result) {
      ::close(input_pipe[1]);
      ::close(output_pipe[0]);
      throw std::runtime_error("cannot start CLI: " +
                               std::string(std::strerror(result)));
    }
    int status = 0;
    size_t position = 0;
    std::vector<std::string> replies;
    auto* stream = ::fdopen(output_pipe[0], "r");
    if (!stream) {
      ::close(input_pipe[1]);
      ::close(output_pipe[0]);
      while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      throw std::runtime_error("cannot open CLI output stream");
    }
    char* line = nullptr;
    size_t capacity = 0;
    const auto read_reply = [&] {
      const auto count = ::getline(&line, &capacity, stream);
      if (count < 0) return false;
      if (count > 1) replies.emplace_back(line, count - 1);
      return true;
    };
    while (position < input.size()) {
      const auto newline = input.find('\n', position);
      const auto end =
          newline == std::string::npos ? input.size() : newline + 1;
      // The first request is Hello, and every subsequent request must
      // observe the preceding reply before it is sent.
      while (position < end) {
        const auto count =
            ::write(input_pipe[1], input.data() + position, end - position);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        position += static_cast<size_t>(count);
      }
      if (position != end) break;
      if (position < input.size() && !read_reply()) break;
    }
    ::close(input_pipe[1]);
    while (read_reply()) {
    }
    std::free(line);
    ::fclose(stream);
    while (::waitpid(pid, &status, 0) < 0) {
      if (errno != EINTR) throw std::runtime_error("cannot wait for CLI");
    }
    CliResult output{
        WIFEXITED(status) ? WEXITSTATUS(status) : -1, Read(stderr_path),
        std::move(replies)};
    if (output.status)
      for (const auto& reply : output.replies) output.error += reply + '\n';
    return output;
  }

  TemporaryDirectory directory;
  ChildProcess project;
};

TEST_P(RpcCli, OrdersUnionsWithinProjectChangeArrays) {
  Write(
      directory.path / "project/tsugumi.json",
      tsugumi::test::project_json(
          R"({"name":"Before","nixPath": [],"canvas":{"cards":[],"connections":[]}})"));
  project.Start({TSUGUMI_PROJECT_BINARY, "--scratch-dir",
                 (directory.path / "scratch").string(), "--run-scratch-dir",
                 (directory.path / "run-scratch").string(), "--project",
                 (directory.path / "project").string(), "--unix",
                 (directory.path / "project.sock").string(), "--listen",
                 "127.0.0.1:0", "--allow-missing-origin", "--control-dir",
                 (directory.path / "project-control").string(), "--worker",
                 TSUGUMI_WORKER_BINARY, "--nix-dir", TSUGUMI_TEST_NIX},
                directory.path / "project.log");
  const auto server =
      Readiness(project, directory.path / "project-control/server.ready",
                directory.path / "project.log");
  const auto hello =
      Request(1, "SessionHelloRequest",
              "{\"token\":" + Quote(JsonString(server, "/token")) + "}");
  const auto inspect = Run(hello + Request(2, "ProjectInspectRequest", "{}"),
                           Endpoint(server), TSUGUMI_PROJECT_SCHEMA);
  ASSERT_EQ(inspect.status, 0) << inspect.error;
  ASSERT_EQ(inspect.replies.size(), 2);
  ASSERT_EQ(JsonString(inspect.replies[1], "/payload/value_type"),
            "ProjectSnapshot")
      << inspect.replies[1];
  const auto revision = JsonUInt(inspect.replies[1], "/payload/value/rev");
  const std::string document = tsugumi::test::project_json(
      R"({"name":"After 日本語","nixPath": [],"canvas":{"cards":[],"connections":[]}})");
  const auto save =
      "{\"changes\":[{\"value\":{\"json\":" + Quote(document) +
      "},\"value_type\":\"SetProject\"}],\"origin\":\"user\",\"op_id\":"
      "\"631f717a-dbe6-4ad4-aa1f-f4e6a627b2b6\",\"base_rev\":" +
      std::to_string(revision) + "}";
  const auto result = Run(hello + Request(2, "ProjectSaveRequest", save) +
                              Request(3, "ProjectInspectRequest", "{}"),
                          Endpoint(server), TSUGUMI_PROJECT_SCHEMA);
  ASSERT_EQ(result.status, 0) << result.error;
  ASSERT_EQ(result.replies.size(), 3);
  EXPECT_EQ(JsonString(result.replies[1], "/payload/value_type"), "SaveResult");
  EXPECT_EQ(
      JsonString(JsonString(result.replies[2], "/payload/value/project_json"),
                 "/name"),
      "After 日本語");
}

// Links to deep folders go to the test's own folder; the service and the CLI
// inherit the setting.
class SocketDirectory {
 public:
  explicit SocketDirectory(const std::filesystem::path& path) {
    if (const auto* value = std::getenv("TSUGUMI_SOCKET_DIR"))
      previous_ = value;
    ::setenv("TSUGUMI_SOCKET_DIR", path.c_str(), 1);
  }
  ~SocketDirectory() {
    if (previous_)
      ::setenv("TSUGUMI_SOCKET_DIR", previous_->c_str(), 1);
    else
      ::unsetenv("TSUGUMI_SOCKET_DIR");
  }

 private:
  std::optional<std::string> previous_;
};

// A project service in a folder deeper than sun_path allows still listens on
// its own socket, and a native client reaches it through the same path.
class RpcCliDeepSocket : public RpcCli {};

TEST_P(RpcCliDeepSocket, ServesAndConnectsInAFolderDeeperThanSunPath) {
  const SocketDirectory sockets(directory.path);
  auto deep = directory.path;
  for (int i = 0; i < 8; ++i) deep /= std::string(20, 'd');
  std::filesystem::create_directories(deep);
  const auto socket = (deep / "project.sock").string();
  ASSERT_GT(socket.size(), sizeof(sockaddr_un::sun_path));
  Write(
      directory.path / "project/tsugumi.json",
      tsugumi::test::project_json(
          R"({"name":"Deep","nixPath": [],"canvas":{"cards":[],"connections":[]}})"));
  project.Start(
      {TSUGUMI_PROJECT_BINARY, "--scratch-dir",
       (directory.path / "scratch").string(), "--run-scratch-dir",
       (directory.path / "run-scratch").string(), "--project",
       (directory.path / "project").string(), "--unix", socket, "--listen",
       "127.0.0.1:0", "--allow-missing-origin", "--control-dir",
       (directory.path / "project-control").string(), "--worker",
       TSUGUMI_WORKER_BINARY, "--nix-dir", TSUGUMI_TEST_NIX},
      directory.path / "project.log");
  const auto server =
      Readiness(project, directory.path / "project-control/server.ready",
                directory.path / "project.log");
  // The socket file is where it was asked for, not in the link folder.
  EXPECT_EQ(JsonString(server, "/unix_socket"), socket);
  EXPECT_TRUE(std::filesystem::is_socket(socket));
  const auto hello =
      Request(1, "SessionHelloRequest",
              "{\"token\":" + Quote(JsonString(server, "/token")) + "}");
  const auto inspect = Run(hello + Request(2, "ProjectInspectRequest", "{}"),
                           Endpoint(server), TSUGUMI_PROJECT_SCHEMA);
  ASSERT_EQ(inspect.status, 0) << inspect.error;
  ASSERT_EQ(inspect.replies.size(), 2);
  EXPECT_EQ(JsonString(inspect.replies[1], "/payload/value_type"),
            "ProjectSnapshot")
      << inspect.replies[1];
}

INSTANTIATE_TEST_SUITE_P(Transports, RpcCliDeepSocket, ::testing::Values(false),
                         [](const auto&) { return "UnixSocket"; });

TEST(UnixSocketAddress, NamesThePathAndTheLimitWhenANameCannotBeShortened) {
  TemporaryDirectory directory;
  const auto path = (directory.path / std::string(120, 'n')).string();
  try {
    unix_socket::Address(path);
    FAIL() << "a 120-byte name cannot fit in sun_path";
  } catch (const std::runtime_error& error) {
    const std::string message = error.what();
    EXPECT_NE(message.find(path), std::string::npos) << message;
    EXPECT_NE(message.find("this system allows " +
                           std::to_string(unix_socket::kMaxPath)),
              std::string::npos)
        << message;
  }
}

INSTANTIATE_TEST_SUITE_P(Transports, RpcCli, ::testing::Bool(),
                         [](const auto& info) {
                           return info.param ? "WebSocket" : "UnixSocket";
                         });

}  // namespace
}  // namespace tsugumi::test
