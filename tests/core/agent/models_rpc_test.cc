#include "fixture.hh"

namespace tsugumi::test {
namespace {
class ModelsFixture : public AgentFixture {
 protected:
  auto Models(std::string id, const ModelServer* server = nullptr,
              bool websocket = false) -> Reply {
    if (server) {
      const auto configured =
          Call(*host, H::RequestPayload::ProviderSetRequest, [&](Builder& b) {
            Tsugumi::Service::ProviderInputT config;
            config.endpoint = server->URL();
            config.api_key = "MODEL_KEY_SENTINEL";
            return H::CreateProviderSetRequest(
                       b, b.CreateString("desktop"), b.CreateString(id),
                       Tsugumi::Service::ProviderInput::Pack(b, &config))
                .Union();
          });
      if (!configured.Code().empty())
        throw std::runtime_error("cannot configure model catalog");
    }
    auto connection = websocket ? ConnectHost(true) : nullptr;
    return Call(connection ? *connection : *host,
                H::RequestPayload::ModelsListRequest, [&](Builder& b) {
                  Tsugumi::Service::ProviderInputT config;
                  if (server) config.endpoint = server->URL();
                  return H::CreateModelsListRequest(b, b.CreateString(id),
                                                    server != nullptr,
                                                    b.CreateString("desktop"))
                      .Union();
                });
  }
};
TEST_F(ModelsFixture, StoredReadsNeedNoProviderAndAnAbsentVectorIsEmpty) {
  const auto reply = Models("anthropic", nullptr, true);
  ASSERT_TRUE(reply.Code().empty());
  EXPECT_EQ(ListSize(reply.HostValue()->value_as_ModelList()->models()), 0u);
}
TEST_F(ModelsFixture,
       RefreshPersistsDisplayNamesAndEmptyOrFailedRefreshKeepsThem) {
  ModelServer provider;
  provider.QueueHTTP(
      R"({"data":[{"id":"model-a","display_name":"Model A"},{"id":"model-b"}]})");
  const auto fresh = Models("anthropic", &provider);
  ASSERT_TRUE(fresh.Code().empty());
  EXPECT_EQ(JsonString(fresh.View(), "/0/name"), "Model A");
  EXPECT_EQ(JsonString(fresh.View(), "/1/name"), "model-b");
  const auto saved = fresh.View();
  EXPECT_EQ(Models("anthropic").View(), saved);
  EXPECT_EQ(provider.Requests().size(), 1u);
  EXPECT_EQ(Models("openai").View(), "[]");
  provider.QueueHTTP(R"({"data":[]})");
  EXPECT_EQ(Models("anthropic", &provider, true).View(), saved);
  provider.QueueHTTP(R"({"error":{"message":"offline"}})", 503);
  EXPECT_FALSE(Models("anthropic", &provider).Code().empty());
  EXPECT_EQ(Models("anthropic").View(), saved);
  host.reset();
  external.reset();
  human.reset();
  agent.Stop();
  StartAgent();
  EXPECT_EQ(Models("anthropic", nullptr, true).View(), saved);
}
TEST_F(ModelsFixture, ConcurrentRefreshesKeepSeparateProviderCatalogs) {
  ModelServer first, second;
  first.QueueHTTP(R"({"data":[{"id":"a","display_name":"A"}]})");
  second.QueueHTTP(R"({"data":[{"id":"b","display_name":"B"}]})");
  auto a = std::async(std::launch::async,
                      [&] { return Models("anthropic", &first, true); });
  auto b = std::async(std::launch::async,
                      [&] { return Models("openai", &second, true); });
  ASSERT_TRUE(a.get().Code().empty());
  ASSERT_TRUE(b.get().Code().empty());
  EXPECT_EQ(JsonString(Models("anthropic").View(), "/0/id"), "a");
  EXPECT_EQ(JsonString(Models("openai").View(), "/0/id"), "b");
}
}  // namespace
}  // namespace tsugumi::test
