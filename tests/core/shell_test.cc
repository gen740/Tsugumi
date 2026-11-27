#include "shell.hh"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "tsugumi/util/fs.hh"
#include "tsugumi/util/process.hh"

namespace tsugumi::shell {
namespace {
class ShellTest : public testing::Test {
 protected:
  std::filesystem::path root;
  std::filesystem::path shared_note;
  std::atomic<bool> cancelled = false;
  auto SetUp() -> void override {
    auto pattern =
        (std::filesystem::temp_directory_path() / "tsugumi-shell-test-XXXXXX")
            .string();
    ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
    root = std::filesystem::canonical(pattern);
    shared_note = root.string() + "-shared-note";
    fs::write_atomic(shared_note, "shared temporary data");
    fs::write_atomic(root / "note", "original");
    std::filesystem::create_directory(root / "control");
    fs::write_atomic(root / "control/token", "secret");
  }
  auto TearDown() -> void override {
    std::filesystem::remove_all(root);
    std::filesystem::remove(shared_note);
  }
  auto Run(const std::string& command, bool read_only = false,
           bool hide_credentials = false, bool no_project_socket = false)
      -> Output {
    return Execute(command, root.string(), 1000, cancelled, read_only,
                   hide_credentials, no_project_socket, root / "control");
  }
};
TEST_F(ShellTest, UnrestrictedRequestCanWriteAndReadControl) {
  const auto output = Run("printf changed > note; cat control/token");
  EXPECT_EQ(WEXITSTATUS(output.status), 0);
  EXPECT_EQ(output.streams[0], "secret");
  EXPECT_EQ(fs::read_file(root / "note"), "changed");
}
TEST_F(ShellTest, ChildSetupFailuresReportTheOperationPathAndErrno) {
  const auto diagnostic = root / "setup-error";
  const auto missing = (root / "missing").string();
  for (const bool missing_cwd : {true, false}) {
    const int fd = ::open(diagnostic.c_str(),
                          O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    ASSERT_GE(fd, 0);
    proc::Spec spec;
    spec.argv = {missing};
    spec.cwd = missing_cwd ? missing : root.string();
    spec.stderr_fd = fd;
    EXPECT_THROW(proc::spawn(spec), std::runtime_error);
    ::close(fd);
    const auto text = fs::read_file_or_throw(diagnostic);
    EXPECT_NE(text.find(missing_cwd ? "chdir " : "execve "), std::string::npos);
    EXPECT_NE(text.find(missing), std::string::npos);
    EXPECT_NE(text.find(std::strerror(ENOENT)), std::string::npos);
  }
}
#ifdef __linux__
TEST_F(ShellTest, ReportsStartupErrorWithHighMappedDescriptor) {
  constexpr int child_fd = 1100;
  rlimit limit{};
  ASSERT_EQ(::getrlimit(RLIMIT_NOFILE, &limit), 0);
  const auto required = static_cast<rlim_t>(child_fd + 3);
  struct RestoreLimit {
    rlimit old;
    bool changed = false;
    ~RestoreLimit() {
      if (changed) EXPECT_EQ(::setrlimit(RLIMIT_NOFILE, &old), 0);
    }
  } restore{limit};
  if (limit.rlim_cur < required) {
    if (limit.rlim_max < required) GTEST_SKIP() << "descriptor limit too low";
    limit.rlim_cur = required;
    ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &limit), 0);
    restore.changed = true;
  }
  const auto diagnostic = root / "startup-error";
  const int fd = ::open(diagnostic.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
  ASSERT_GE(fd, 0);
  proc::Spec spec;
  spec.argv = {(root / "missing-executable").string()};
  spec.stderr_fd = fd;
  spec.fds = {{child_fd, fd}};
  EXPECT_THROW({ proc::wait(proc::spawn(spec)); }, std::runtime_error);
  ::close(fd);
  EXPECT_NE(fs::read_file_or_throw(diagnostic).find("execve "), std::string::npos);
}
#endif

TEST(Process, EnforcesDeadlineAfterOutputCloses) {
  using namespace std::chrono_literals;
  proc::Spec spec;
  spec.argv = {"/bin/sh", "-c",
               "trap '' TERM; exec 1>&- 2>&-; exec /bin/sleep 3"};
  const auto result = proc::run(spec, nullptr,
                                std::chrono::steady_clock::now() + 500ms,
                                {}, std::nullopt, 50ms);
  EXPECT_TRUE(result.timed_out);
  ASSERT_TRUE(WIFSIGNALED(result.status));
  EXPECT_EQ(WTERMSIG(result.status), SIGKILL);
}

TEST(Process, AStoppedChildWithClosedOutputStillHasADeadline) {
  using namespace std::chrono_literals;
  proc::Spec spec;
  // The helper resumes the child eventually so a regression fails the
  // assertions without leaving a stopped process behind.
  spec.argv = {"/bin/sh", "-c",
               "trap '' TERM; "
               "(/bin/sleep 2; kill -CONT \"$$\") >/dev/null 2>&1 & "
               "exec 1>&- 2>&-; kill -STOP \"$$\"; exit 42"};
  const auto result =
      proc::run(spec, nullptr, std::chrono::steady_clock::now() + 500ms, {},
                std::nullopt, 50ms);
  EXPECT_TRUE(result.timed_out);
  ASSERT_TRUE(WIFSIGNALED(result.status));
  EXPECT_EQ(WTERMSIG(result.status), SIGKILL);
}

TEST_F(ShellTest, ReadOnlyIsIndependentOfCredentialHiding) {
  const auto output = Run("cat control/token; cat '" + shared_note.string() +
                              "'; printf changed > note",
                          true);
  if (output.streams[1].find("sandbox_apply: Operation not permitted") !=
      std::string::npos)
    GTEST_SKIP() << "The outer sandbox disallows starting Seatbelt";
  EXPECT_NE(WEXITSTATUS(output.status), 0);
  EXPECT_EQ(output.streams[0], "secretshared temporary data");
  EXPECT_EQ(fs::read_file(root / "note"), "original");
}
TEST_F(ShellTest, HiddenCredentialsDoNotMakeTheProjectReadOnly) {
  const auto output =
      Run("printf changed > note; cat control/token", false, true);
  if (output.streams[1].find("sandbox_apply: Operation not permitted") !=
      std::string::npos)
    GTEST_SKIP() << "The outer sandbox disallows starting Seatbelt";
  EXPECT_NE(WEXITSTATUS(output.status), 0);
  EXPECT_EQ(output.streams[0], "");
  EXPECT_EQ(fs::read_file(root / "note"), "changed");
}
TEST_F(ShellTest, SocketRestrictionDoesNotHideCredentialsOrPreventFileEdits) {
  const auto output =
      Run("printf changed > note; cat control/token", false, false, true);
  if (output.streams[1].find("sandbox_apply: Operation not permitted") !=
      std::string::npos)
    GTEST_SKIP() << "The outer sandbox disallows starting Seatbelt";
  EXPECT_EQ(WEXITSTATUS(output.status), 0);
  EXPECT_EQ(output.streams[0], "secret");
  EXPECT_EQ(fs::read_file(root / "note"), "changed");
}
TEST_F(ShellTest, AllRestrictionsPreserveDiagnosticReadsAndRejectWrites) {
  const auto output = Run("cat note; printf changed > note", true, true, true);
  if (output.streams[1].find("sandbox_apply: Operation not permitted") !=
      std::string::npos)
    GTEST_SKIP() << "The outer sandbox disallows starting Seatbelt";
  EXPECT_EQ(output.streams[0], "original");
  EXPECT_NE(WEXITSTATUS(output.status), 0);
  EXPECT_EQ(fs::read_file(root / "note"), "original");
}
}  // namespace
}  // namespace tsugumi::shell
