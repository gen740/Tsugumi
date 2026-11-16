// The production wire helper supports absent Host lists.
#include <gtest/gtest.h>

#include "agent/wire.hh"

namespace tsugumi::host {
namespace {
namespace A = Tsugumi::Agent;
namespace H = Tsugumi::Host;
namespace S = Tsugumi::Service;

TEST(ServiceContracts, BothRepliesUseOneTextResultAndHostListsMayBeOmitted) {
  S::TextResultT text;
  text.text = "summary";
  text.usage = std::make_unique<S::UsageT>();
  text.usage->input_tokens = 3;
  const auto agent = agent::wire::AgentReply(7, text);
  const auto host = agent::wire::HostReply(7, text);
  const auto* a =
      A::GetMessage(agent.data())->payload_as_Reply()->value_as_TextResult();
  const auto* h =
      H::GetMessage(host.data())->payload_as_Reply()->value_as_TextResult();
  ASSERT_NE(a, nullptr);
  ASSERT_NE(h, nullptr);
  EXPECT_EQ(a->text()->str(), h->text()->str());
  EXPECT_EQ(a->usage()->input_tokens(), h->usage()->input_tokens());
  const auto empty = agent::wire::HostReply(8, H::DemoPathsT{});
  flatbuffers::Verifier verifier(reinterpret_cast<const uint8_t*>(empty.data()),
                                 empty.size());
  ASSERT_TRUE(H::VerifyMessageBuffer(verifier));
  const auto* paths =
      H::GetMessage(empty.data())->payload_as_Reply()->value_as_DemoPaths();
  ASSERT_NE(paths, nullptr);
  EXPECT_EQ(paths->paths(), nullptr);
}
}  // namespace
}  // namespace tsugumi::host
