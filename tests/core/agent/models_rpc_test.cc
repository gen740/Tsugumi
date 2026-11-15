#include "fixture.hh"

namespace tsugumi::test {
namespace {
class ModelsFixture : public AgentFixture {
 protected:
  auto Models(std::string id, const ModelServer* server = nullptr,
              bool websocket = false, std::string key_profile = {}) -> Reply {
    if (server) {
      const auto configured =
          Call(*host, H::RequestPayload::ProviderSetRequest, [&](Builder& b) {
            Tsugumi::Host::ProviderInputT config;
            config.endpoint = server->URL();
            return H::CreateProviderSetRequest(
                       b, b.CreateString("desktop"), b.CreateString(id),
                       Tsugumi::Host::ProviderInput::Pack(b, &config))
                .Union();
          });
      if (!configured.Code().empty())
        throw std::runtime_error("cannot configure model catalog");
    }
    if (server) {
      const auto provisioned =
          Call(*host, H::RequestPayload::ProviderSetRequest, [&](Builder& b) {
            Tsugumi::Host::ProviderInputT key;
            key.api_key = "MODEL_KEY_SENTINEL";
            return H::CreateProviderSetRequest(
                       b,
                       b.CreateString(key_profile.empty() ? "desktop"
                                                         : key_profile),
                       b.CreateString(id),
                       Tsugumi::Host::ProviderInput::Pack(b, &key), false,
                       true)
                .Union();
          });
      if (!provisioned.Code().empty())
        throw std::runtime_error("cannot provision model catalog key");
    }
    auto connection = websocket ? ConnectHost(true) : nullptr;
    return Call(connection ? *connection : *host,
                H::RequestPayload::ModelsListRequest, [&](Builder& b) {
                  Tsugumi::Host::ProviderInputT config;
                  if (server) config.endpoint = server->URL();
                  return H::CreateModelsListRequest(
                             b, b.CreateString(id), server != nullptr,
                             b.CreateString("desktop"), {},
                             b.CreateString(key_profile))
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
      R"({"data":[{"id":"model-a","display_name":"Model A","max_input_tokens":1000000},{"id":"model-b","max_input_tokens":null}]})");
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
  service.Stop();
  StartHost();
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

TEST_F(ModelsFixture, SameProviderConnectionsKeepTheirOwnCatalogs) {
  ModelServer provider;
  provider.QueueHTTP(
      R"({"data":[{"id":"shared","display_name":"First connection","max_input_tokens":1000000}]})");
  ASSERT_TRUE(Models("anthropic", &provider, false, "first").Code().empty());
  provider.QueueHTTP(
      R"({"data":[{"id":"shared","display_name":"Second connection","max_input_tokens":128000}]})");
  ASSERT_TRUE(Models("anthropic", &provider, false, "second").Code().empty());
  EXPECT_EQ(
      JsonString(Models("anthropic", nullptr, true, "first").View(), "/0/name"),
      "First connection");
  EXPECT_EQ(JsonString(Models("anthropic", nullptr, true, "second").View(),
                       "/0/name"),
            "Second connection");
  EXPECT_EQ(Models("anthropic").View(), "[]");
  host.reset();
  external.reset();
  human.reset();
  service.Stop();
  StartHost();
  EXPECT_EQ(
      JsonString(Models("anthropic", nullptr, true, "first").View(), "/0/name"),
      "First connection");
  EXPECT_EQ(JsonString(Models("anthropic", nullptr, true, "second").View(),
                       "/0/name"),
            "Second connection");
}
}  // namespace
}  // namespace tsugumi::test
