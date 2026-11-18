#include "shell.hh"

#include <fcntl.h>
#include <gtest/gtest.h>
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
