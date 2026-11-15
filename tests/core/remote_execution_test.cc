#include <gtest/gtest.h>

#include "service.hh"

namespace tsugumi {
namespace {
TEST(RemoteExecutionCapabilities, CachedLimitsAndSystemsCannotRefuseARunBeforeProbe) {
  Node node;
  node.path = {"work"};
  node.settings.execution_service = "worker";
  node.settings.system = "aarch64-linux";
  node.settings.execution_resources = ExecutionResources{500, 256u << 20};
  RemoteTarget cached;
  cached.id = "worker";
  cached.available = true;
  cached.native_system = "x86_64-linux";
  cached.systems = {cached.native_system};
  cached.resource_limits = false;
  EXPECT_FALSE(Service::execution_diagnostic(node, {cached}));
}
TEST(RemoteExecutionCapabilities, MissingOrRevokedRegistrationStillRefusesARun) {
  Node node;
  node.path = {"work"};
  node.settings.execution_service = "worker";
  EXPECT_TRUE(Service::execution_diagnostic(node, {}));
  RemoteTarget revoked;
  revoked.id = "worker";
  revoked.available = false;
  EXPECT_TRUE(Service::execution_diagnostic(node, {revoked}));
}
}  // namespace
}  // namespace tsugumi
