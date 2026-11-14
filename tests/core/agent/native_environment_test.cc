#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../agent_fixture_server.hh"
#include "host/environment.hh"

namespace tsugumi::test {
TEST(NativeEnvironment, StartsWithOnlyRuntimeEnvironmentAndScopedBroker) {
  TemporaryDirectory directory;
  // Poison known credentials, an unknown future secret and shell injection.
  const std::vector<std::string> names = {
      "OPENAI_API_KEY",
      "ANTHROPIC_API_KEY",
      "FUTURE_PROVIDER_SECRET",
      "BASH_ENV",
      "HOME",
      "PATH",
      "TMPDIR",
      "LANG",
      "NIX_CONFIG",
      "SSL_CERT_FILE",
      "NIX_SSL_CERT_FILE",
      "TSUGUMI_EXECUTION_BROKER",
      "TSUGUMI_EXECUTION_BROKER_CAPABILITY_FILE"};
  std::vector<std::optional<std::string>> previous;
  for (const auto &name : names) {
    const char *value = std::getenv(name.c_str());
    previous.push_back(value ? std::optional<std::string>(value)
                             : std::nullopt);
    ASSERT_EQ(::setenv(name.c_str(), "host-secret", 1), 0);
  }
  struct Restore {
    const std::vector<std::string> &names;
    const std::vector<std::optional<std::string>> &previous;
    ~Restore() {
      for (size_t i = 0; i < names.size(); ++i) {
        if (previous[i])
          ::setenv(names[i].c_str(), previous[i]->c_str(), 1);
        else
          ::unsetenv(names[i].c_str());
      }
    }
  } restore{names, previous};
  host::EnvironmentLaunch launch;
  launch.arguments = {"/usr/bin/env"};
  launch.log = directory.path / "environment";
  auto process = host::NativeEnvironmentProcess();
  for (const bool authorized : {false, true}) {
    launch.authorization.available = authorized;
    launch.authorization.endpoint = "unix:project-broker";
    launch.authorization.capability_file = "/control/project-capability";
    const auto pid = process->Launch(launch);
    int status = 0;
    ASSERT_EQ(::waitpid(static_cast<pid_t>(pid), &status, 0), pid);
    ASSERT_EQ(status, 0);
    const auto output = Read(launch.log);
    EXPECT_EQ(output.find("host-secret"), std::string::npos);
    EXPECT_NE(output.find("PATH="), std::string::npos);
    EXPECT_NE(output.find("HOME="), std::string::npos);
    EXPECT_NE(output.find("LANG=C.UTF-8"), std::string::npos);
    EXPECT_EQ(output.find("TSUGUMI_EXECUTION_BROKER=unix:project-broker") !=
                  std::string::npos,
              authorized);
    EXPECT_EQ(output.find("TSUGUMI_EXECUTION_BROKER_CAPABILITY_FILE=/control/"
                          "project-capability") != std::string::npos,
              authorized);
    std::filesystem::remove(launch.log);
  }
}
} // namespace tsugumi::test
