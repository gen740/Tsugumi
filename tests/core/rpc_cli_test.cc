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
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "agent_fixture_server.hh"

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
  auto SetUp() -> void override {
    std::filesystem::create_directories(directory.path / "demos");
    agent.Start(
        {TSUGUMI_AGENT_BINARY, "--state-dir",
         (directory.path / "state").string(), "--unix",
         (directory.path / "agent.sock").string(), "--demos-dir",
         (directory.path / "demos").string(), "--listen", "127.0.0.1:0"},
        directory.path / "agent.log");
    ready = Readiness(agent, directory.path / "state/agent.ready",
                      directory.path / "agent.log");
  }

  auto Endpoint(const std::string& server) const -> std::string {
    return GetParam() ? JsonString(server, "/endpoint")
                      : "unix:" + JsonString(server, "/unix_socket");
  }

  auto Run(const std::string& input, const std::string& endpoint,
           const std::string& schema) -> CliResult {
    const auto stdout_path = directory.path / "cli.output";
    const auto stderr_path = directory.path / "cli.error";
    int input_pipe[2];
    if (::pipe(input_pipe)) throw std::runtime_error("cannot create CLI input");
    const std::vector<std::string> arguments{
        TSUGUMI_RPC_BINARY, "--endpoint", endpoint, "--schema", schema,
        "--timeout-ms",     "3000"};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, input_pipe[0], STDIN_FILENO);
    posix_spawn_file_actions_addclose(&actions, input_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, input_pipe[1]);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
                                     stdout_path.c_str(),
                                     O_CREAT | O_WRONLY | O_TRUNC, 0600);
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
    if (result) {
      ::close(input_pipe[1]);
      throw std::runtime_error("cannot start CLI: " +
                               std::string(std::strerror(result)));
    }
    const auto deadline = std::chrono::steady_clock::now() + 6s;
    int status = 0;
    size_t position = 0;
    size_t sent = 0;
    while (true) {
      const auto waited = ::waitpid(pid, &status, WNOHANG);
      if (waited == pid) break;
      if (waited < 0 && errno != EINTR)
        throw std::runtime_error("cannot wait for CLI");
      if (std::chrono::steady_clock::now() >= deadline) {
        ::kill(pid, SIGKILL);
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        throw std::runtime_error("CLI did not finish: " + Read(stderr_path));
      }
      const auto output = Read(stdout_path);
      // A project Hello must be acknowledged before another operation. Each
      // following operation likewise observes the preceding public reply.
      if (position < input.size() &&
          static_cast<size_t>(std::count(output.begin(), output.end(), '\n')) >=
              sent) {
        const auto newline = input.find('\n', position);
        const auto end =
            newline == std::string::npos ? input.size() : newline + 1;
        while (position < end) {
          const auto count =
              ::write(input_pipe[1], input.data() + position, end - position);
          if (count < 0 && errno == EINTR) continue;
          if (count <= 0) throw std::runtime_error("cannot write CLI input");
          position += static_cast<size_t>(count);
        }
        ++sent;
      }
      if (position == input.size() && input_pipe[1] != -1) {
        ::close(input_pipe[1]);
        input_pipe[1] = -1;
      }
      std::this_thread::sleep_for(5ms);
    }
    if (input_pipe[1] != -1) ::close(input_pipe[1]);
    CliResult output{
        WIFEXITED(status) ? WEXITSTATUS(status) : -1, Read(stderr_path), {}};
    std::istringstream lines(Read(stdout_path));
    for (std::string line; std::getline(lines, line);)
      if (!line.empty()) output.replies.push_back(std::move(line));
    if (output.status)
      for (const auto& reply : output.replies) output.error += reply + '\n';
    return output;
  }

  auto Hello() const -> std::string {
    return "{\"client_id\":\"CLI JSON order scenario\",\"token\":" +
           Quote(JsonString(ready, "/user_token")) + "}";
  }

  TemporaryDirectory directory;
  ChildProcess agent;
  ChildProcess project;
  std::string ready;
};

TEST_P(RpcCli, AcceptsEveryEnvelopeAndNestedUnionOrderWithoutRoundingIds) {
  std::array<int, 3> order{0, 1, 2};
  uint64_t id = std::numeric_limits<uint64_t>::max() - 24;
  do {
    for (const bool type_first : {false, true}) {
      const auto payload =
          type_first
              ? "{\"value_type\":\"HelloRequest\",\"value\":" + Hello() + "}"
              : "{\"value\":" + Hello() + ",\"value_type\":\"HelloRequest\"}";
      const std::array<std::string, 3> members{
          "\"payload\":" + payload, "\"request_id\":" + std::to_string(id),
          "\"payload_type\":\"Request\""};
      auto input = "{" + members[order[0]] + "," + members[order[1]] + "," +
                   members[order[2]] + "}\n" +
                   Request(id + 1, "ToolCatalogRequest", "{}");
      SCOPED_TRACE(input);
      const auto result = Run(input, Endpoint(ready), TSUGUMI_AGENT_SCHEMA);
      ASSERT_EQ(result.status, 0) << result.error;
      ASSERT_EQ(result.replies.size(), 2);
      EXPECT_EQ(JsonUInt(result.replies[0], "/request_id"), id);
      EXPECT_EQ(JsonUInt(result.replies[1], "/request_id"), id + 1);
      EXPECT_EQ(JsonString(result.replies[0], "/payload/value_type"),
                "HelloResult");
      EXPECT_EQ(JsonString(result.replies[1], "/payload/value_type"), "Result");
      id += 2;
    }
  } while (std::next_permutation(order.begin(), order.end()));
}

TEST_P(RpcCli, PreservesEscapedKeysAndOpaqueJsonStrings) {
  const auto id = std::string("Japanese 日本語: \"value_type\", \\\\ ") +
                  "{\"n\":18446744073709551615,\"s\":-9223372036854775808}";
  auto hello = Request(1, "HelloRequest", Hello());
  hello.replace(hello.find("payload_type"), 12, "pay\\u006coad_type");
  const auto result =
      Run(hello + Request(2, "ConversationCreateRequest",
                          "{\"conversation_id\":" + Quote(id) + "}"),
          Endpoint(ready), TSUGUMI_AGENT_SCHEMA);
  ASSERT_EQ(result.status, 0) << result.error;
  ASSERT_EQ(result.replies.size(), 2);
  EXPECT_EQ(JsonString(JsonString(result.replies[1], "/payload/value/json"),
                       "/conversation/id"),
            id);
}

TEST_P(RpcCli, OrdersUnionsWithinProjectChangeArrays) {
  Write(
      directory.path / "project/tsugumi.json",
      R"({"name":"Before","pkgsPath":"tsugumi:default","canvas":{"cards":[],"connections":[]}})");
  Write(directory.path / "project/layout.json", "{}");
  project.Start({TSUGUMI_PROJECT_BINARY, "--project",
                 (directory.path / "project").string(), "--unix",
                 (directory.path / "project.sock").string(), "--listen",
                 "127.0.0.1:0", "--allow-missing-origin", "--control-dir",
                 (directory.path / "project-control").string(), "--worker",
                 TSUGUMI_WORKER_BINARY, "--sdk-dir", TSUGUMI_TEST_SDK},
                directory.path / "project.log");
  const auto server =
      Readiness(project, directory.path / "project-control/server.ready",
                directory.path / "project.log");
  const auto hello = Request(1, "SessionHelloRequest",
                             "{\"protocol\":1,\"client_id\":\"1a363cee-72b1-"
                             "4713-8e1c-251354239be5\",\"token\":" +
                                 Quote(JsonString(server, "/token")) + "}");
  const auto inspect = Run(hello + Request(2, "ProjectInspectRequest", "{}"),
                           Endpoint(server), TSUGUMI_PROJECT_SCHEMA);
  ASSERT_EQ(inspect.status, 0) << inspect.error;
  ASSERT_EQ(inspect.replies.size(), 2);
  ASSERT_EQ(JsonString(inspect.replies[1], "/payload/value_type"),
            "ProjectSnapshot")
      << inspect.replies[1];
  const auto revision = JsonUInt(inspect.replies[1], "/payload/value/rev");
  const std::string document =
      R"({"name":"After 日本語","pkgsPath":"tsugumi:default","canvas":{"cards":[],"connections":[]}})";
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

TEST_P(RpcCli, RejectsAmbiguousOrTruncatedJsonInput) {
  const auto hello = Request(1, "HelloRequest", Hello());
  const std::vector<std::string> invalid{
      "{\"payload_type\":\"Reply\"," + hello.substr(1),
      hello.substr(0, hello.size() - 1) + '\0' + "false\n",
      hello.substr(0, hello.size() - 1) + "false\n"};
  for (const auto& input : invalid) {
    SCOPED_TRACE(input);
    const auto result = Run(input, Endpoint(ready), TSUGUMI_AGENT_SCHEMA);
    EXPECT_NE(result.status, 0);
    EXPECT_TRUE(result.replies.empty());
    EXPECT_NE(result.error.find("invalid request JSON"), std::string::npos);
  }
}

INSTANTIATE_TEST_SUITE_P(Transports, RpcCli, ::testing::Bool(),
                         [](const auto& info) {
                           return info.param ? "WebSocket" : "UnixSocket";
                         });

}  // namespace
}  // namespace tsugumi::test
