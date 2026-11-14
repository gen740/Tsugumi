#include "oauth_fixture.hh"

namespace tsugumi::test {
namespace {
auto Bytes(const Reply& reply) -> std::string {
  return {reply.bytes.begin(), reply.bytes.end()};
}
TEST_F(AgentFixture, AgentWorkAnimationDefaultsOnAndPersistsAsABoolean) {
  auto animation = [](const Reply& reply) {
    for (const auto* value : *reply.HostValue()->value_as_Settings()->values())
      if (value->key()->str() == "agent.animateWork")
        return value->value_json()->str();
    return std::string{};
  };
  EXPECT_EQ(animation(SettingsGet(*host)), "true");
  EXPECT_FALSE(SettingsPatch(*host, "desktop",
                             {{"agent.animateWork", Quote("false")}})
                   .Code().empty());
  ASSERT_TRUE(SettingsPatch(*host, "desktop", {{"agent.animateWork", "false"}})
                  .Code().empty());
  auto web = ConnectHost(true);
  EXPECT_EQ(animation(SettingsGet(*web)), "false");
  web.reset();
  host.reset();
  human.reset();
  external.reset();
  agent.Stop();
  StartAgent();
  EXPECT_EQ(animation(SettingsGet(*host)), "false");
}
TEST_F(AgentFixture, HelperSettingsAreValidatedSharedAndPersisted) {
  const auto value = [](const Reply &reply, const std::string &key) {
    for (const auto *entry : *reply.HostValue()->value_as_Settings()->values())
      if (entry->key()->str() == key)
        return entry->value_json()->str();
    return std::string{};
  };
  EXPECT_EQ(value(SettingsGet(*host), "agent.helperParallelism"), "4");
  EXPECT_EQ(value(SettingsGet(*host), "agent.allowUnscopedHelpers"), "false");
  for (const auto &invalid : {"0", "9", "1.5", "true", "\"2\""})
    EXPECT_FALSE(
        SettingsPatch(*host, "desktop", {{"agent.helperParallelism", invalid}})
            .Code()
            .empty());
  EXPECT_FALSE(
      SettingsPatch(*host, "desktop", {{"agent.allowUnscopedHelpers", "1"}})
          .Code()
          .empty());
  ASSERT_TRUE(SettingsPatch(*host, "desktop",
                            {{"agent.helperParallelism", "1"},
                             {"agent.allowUnscopedHelpers", "true"}})
                  .Code()
                  .empty());
  auto web = ConnectHost(true);
  EXPECT_EQ(value(SettingsGet(*web), "agent.helperParallelism"), "1");
  EXPECT_EQ(value(SettingsGet(*web), "agent.allowUnscopedHelpers"), "true");
  web.reset();
  host.reset();
  human.reset();
  external.reset();
  agent.Stop();
  StartAgent();
  EXPECT_EQ(value(SettingsGet(*host), "agent.helperParallelism"), "1");
  EXPECT_EQ(value(SettingsGet(*host), "agent.allowUnscopedHelpers"), "true");
}

TEST_F(AgentFixture,
       ConcurrentSettingsUpdatesRejectStaleRevisionsWithoutLosingFields) {
  const auto before = SettingsGet(*host);
  const auto revision = before.HostValue()->value_as_Settings()->revision();
  auto websocket = ConnectHost(true);
  auto update = [&](client::Connection& c, const std::string& key,
                    const std::string& value) {
    return Call(c, H::RequestPayload::SettingsUpdateRequest, [&](Builder& b) {
      const auto entry =
          H::CreateSetting(b, b.CreateString(key), b.CreateString(value));
      return H::CreateSettingsUpdateRequest(b, b.CreateString("desktop"),
                                            revision,
                                            b.CreateVector(std::vector{entry}))
          .Union();
    });
  };
  auto first = std::async(std::launch::async, [&] {
    return update(*host, "agent.model", Quote("first"));
  });
  auto second = std::async(std::launch::async, [&] {
    return update(*websocket, "agent.model.openai", Quote("second"));
  });
  const auto a = first.get(), b = second.get();
  EXPECT_TRUE((a.Code().empty() && b.Code() == "conflict") ||
              (b.Code().empty() && a.Code() == "conflict"));
  ASSERT_TRUE(
      SettingsPatch(*host, "desktop",
                    {{a.Code().empty() ? "agent.model.openai" : "agent.model",
                      Quote("retry after reading")}})
          .Code()
          .empty());
  const auto after = SettingsGet(*host);
  EXPECT_EQ(after.HostValue()->value_as_Settings()->revision(), revision + 2);
  EXPECT_NE(Bytes(after).find("retry after reading"), std::string::npos);
}
TEST_F(AgentFixture, WebPermissionUpdatesKeepTheNativeNixPermission) {
  ASSERT_TRUE(
      SettingsPatch(*host, "desktop",
                    {{"agent.permissions.allowed", R"(["nix_source"])"}})
          .Code()
          .empty());
  const auto before = SettingsGet(*host);
  const auto revision = before.HostValue()->value_as_Settings()->revision();
  const auto updated =
      Call(*host, H::RequestPayload::SettingsUpdateRequest, [&](Builder& b) {
        const auto change =
            H::CreateSetting(b, b.CreateString("agent.permissions.allowed"),
                             b.CreateString(R"(["network"])"));
        return H::CreateSettingsUpdateRequest(
                   b, b.CreateString("desktop"), revision,
                   b.CreateVector(std::vector{change}), b.CreateString("web"))
            .Union();
      });
  ASSERT_TRUE(updated.Code().empty());
  bool found = false;
  for (const auto* value : *updated.HostValue()->value_as_Settings()->values())
    if (value->key()->str() == "agent.permissions.allowed") {
      found = true;
      EXPECT_NE(value->value_json()->str().find("nix_source"),
                std::string::npos);
      EXPECT_NE(value->value_json()->str().find("network"), std::string::npos);
    }
  EXPECT_TRUE(found);
}
TEST_F(AgentFixture,
       ProviderSetAndStatusNeverReturnTheSecretAndSurviveRestart) {
  const std::string key = "PRIVATE_KEY_SENTINEL";
  const auto set =
      Call(*host, H::RequestPayload::ProviderSetRequest, [&](Builder& b) {
        Tsugumi::Service::ProviderInputT p;
        p.api_key = key;
        return H::CreateProviderSetRequest(
                   b, b.CreateString("desktop"), b.CreateString("anthropic"),
                   Tsugumi::Service::ProviderInput::Pack(b, &p))
            .Union();
      });
  ASSERT_TRUE(set.Code().empty());
  EXPECT_TRUE(set.HostValue()->value_as_ProviderStatus()->configured());
  EXPECT_EQ(Bytes(set).find(key), std::string::npos);
  EXPECT_EQ(Bytes(SettingsGet(*host)).find(key), std::string::npos);
  host.reset();
  human.reset();
  external.reset();
  agent.Stop();
  StartAgent();
  const auto status =
      Call(*host, H::RequestPayload::ProviderStatusRequest, [](Builder& b) {
        return H::CreateProviderStatusRequest(b, b.CreateString("desktop"),
                                              b.CreateString("anthropic"))
            .Union();
      });
  EXPECT_TRUE(status.HostValue()->value_as_ProviderStatus()->configured());
  EXPECT_EQ(Bytes(status).find(key), std::string::npos);
  EXPECT_EQ(Read(directory.path / "agent.log").find(key), std::string::npos);
  EXPECT_FALSE(SettingsPatch(*host, "desktop", {{"agent.secret", Quote(key)}})
                   .Code()
                   .empty());
}
TEST_F(
    AgentFixture,
    OAuthVerifiesJWKSAndNeverReturnsTokensAndRotatesOnceAcrossTwoModelCalls) {
  OAuthServer oauth;
  ModelServer model;
  const auto revision =
      SettingsGet(*host).HostValue()->value_as_Settings()->revision();
  const auto signed_in = oauth.SignIn(*host, model.URL(), "desktop", 0);
  ASSERT_EQ(signed_in.HostValue()->value_as_SignInStatus()->state()->str(),
            "complete");
  EXPECT_EQ(SettingsGet(*host).HostValue()->value_as_Settings()->revision(),
            revision);
  for (const auto* secret :
       {"ACCESS_SENTINEL", "REFRESH_SENTINEL", "CODE_SENTINEL"})
    EXPECT_EQ(Bytes(signed_in).find(secret), std::string::npos);
  ASSERT_TRUE(
      SettingsPatch(*host, "desktop", {{"agent.provider", Quote("chatgpt")}})
          .Code()
          .empty());
  oauth.server.QueueHTTP(
      R"({"access_token":"NEW_ACCESS_SENTINEL","refresh_token":"NEW_REFRESH_SENTINEL","expires_in":3600,"scope":"chatgpt.tokens.use.direct","token_type":"Bearer"})");
  model.Queue(ResponsesText("First account call."));
  model.Queue(ResponsesText("Second account call."));
  const auto one = Conversation("refresh-one"),
             two = Conversation("refresh-two");
  ASSERT_TRUE(Send(one, "Respond").Code().empty());
  ASSERT_TRUE(Send(two, "Respond").Code().empty());
  ASSERT_TRUE(model.WaitForRequests(2));
  WaitUntilIdle(one);
  WaitUntilIdle(two);
  EXPECT_EQ(oauth.server.Requests().size(),
            3u);  // code exchange, JWKS, one refresh
  for (const auto& header : model.Headers())
    EXPECT_NE(header.find("NEW_ACCESS_SENTINEL"), std::string::npos);
  oauth.server.QueueHTTP("", 204);
  const auto signout =
      Call(*host, H::RequestPayload::SignInActionRequest, [](Builder& b) {
        return H::CreateSignInActionRequest(b, b.CreateString("desktop"), 0,
                                            b.CreateString("signout"),
                                            b.CreateString("client-desktop"))
            .Union();
      });
  ASSERT_TRUE(signout.Code().empty());
  EXPECT_TRUE(signout.HostValue()->value_as_SignInStatus()->revoked());
  ASSERT_TRUE(oauth.server.WaitForRequests(4));
  EXPECT_NE(oauth.server.Requests()[3].find("NEW_REFRESH_SENTINEL"),
            std::string::npos);
  EXPECT_EQ(oauth.server.Requests()[3].find("token=REFRESH_SENTINEL"),
            std::string::npos);
  for (const auto* secret :
       {"ACCESS_SENTINEL", "REFRESH_SENTINEL", "NEW_ACCESS_SENTINEL",
        "NEW_REFRESH_SENTINEL", "CODE_SENTINEL"}) {
    EXPECT_EQ(Bytes(signout).find(secret), std::string::npos);
    EXPECT_EQ(Events(one).View().find(secret), std::string::npos);
    EXPECT_EQ(Read(directory.path / "agent.log").find(secret),
              std::string::npos);
  }
}
TEST_F(AgentFixture, InvalidOAuthNonceDoesNotInstallAnAccount) {
  OAuthServer oauth;
  ModelServer model;
  oauth.bad_nonce = true;
  const auto result = oauth.SignIn(*host, model.URL());
  EXPECT_EQ(result.HostValue()->value_as_SignInStatus()->state()->str(),
            "failed");
  EXPECT_EQ(ListSize(result.HostValue()->value_as_SignInStatus()->accounts()),
            0u);
  EXPECT_EQ(Bytes(result).find("ACCESS_SENTINEL"), std::string::npos);
}
TEST_F(AgentFixture, InvalidJWKSSignatureDoesNotInstallAnAccount) {
  OAuthServer oauth;
  ModelServer model;
  oauth.bad_signature = true;
  const auto result = oauth.SignIn(*host, model.URL());
  EXPECT_EQ(result.HostValue()->value_as_SignInStatus()->state()->str(),
            "failed");
  EXPECT_EQ(ListSize(result.HostValue()->value_as_SignInStatus()->accounts()),
            0u);
}
TEST_F(AgentFixture, RefreshErrorsCannotEchoCredentialsToRepliesEventsOrLogs) {
  OAuthServer oauth;
  ModelServer model;
  ASSERT_EQ(oauth.SignIn(*host, model.URL(), "desktop", 0)
                .HostValue()
                ->value_as_SignInStatus()
                ->state()
                ->str(),
            "complete");
  ASSERT_TRUE(
      SettingsPatch(*host, "desktop", {{"agent.provider", Quote("chatgpt")}})
          .Code()
          .empty());
  oauth.server.QueueHTTP(
      R"({"error":"invalid_grant","description":"REFRESH_SENTINEL"})", 400);
  const auto conversation = Conversation("rejected-refresh");
  ASSERT_TRUE(Send(conversation, "Respond").Code().empty());
  const auto result = WaitUntilIdle(conversation);
  EXPECT_EQ(model.Requests().size(), 0u);
  EXPECT_EQ(result.find("REFRESH_SENTINEL"), std::string::npos);
  EXPECT_EQ(Events(conversation).View().find("REFRESH_SENTINEL"),
            std::string::npos);
  EXPECT_EQ(Read(directory.path / "agent.log").find("REFRESH_SENTINEL"),
            std::string::npos);
}
TEST_F(AgentFixture, FailedRemoteRevocationStillDeletesTheLocalCredential) {
  OAuthServer oauth;
  ModelServer model;
  ASSERT_EQ(oauth.SignIn(*host, model.URL())
                .HostValue()
                ->value_as_SignInStatus()
                ->state()
                ->str(),
            "complete");
  oauth.server.QueueHTTP(R"({"error":"REFRESH_SENTINEL"})", 503);
  const auto signout =
      Call(*host, H::RequestPayload::SignInActionRequest, [](Builder& b) {
        return H::CreateSignInActionRequest(b, b.CreateString("desktop"), 0,
                                            b.CreateString("signout"),
                                            b.CreateString("client-desktop"))
            .Union();
      });
  ASSERT_TRUE(signout.Code().empty());
  const auto* status = signout.HostValue()->value_as_SignInStatus();
  EXPECT_FALSE(status->revoked());
  ASSERT_EQ(ListSize(status->accounts()), 1u);
  EXPECT_FALSE(status->accounts()->Get(0)->signed_in());
  EXPECT_EQ(Bytes(signout).find("REFRESH_SENTINEL"), std::string::npos);
  EXPECT_EQ(Read(directory.path / "agent.log").find("REFRESH_SENTINEL"),
            std::string::npos);
}
TEST_F(AgentFixture, AppAndWebAccountProfilesRemainSeparate) {
  OAuthServer oauth;
  ModelServer model;
  const auto app = oauth.SignIn(*host, model.URL(), "desktop");
  const auto web = oauth.SignIn(*host, model.URL(), "web:fixture");
  ASSERT_EQ(app.HostValue()->value_as_SignInStatus()->state()->str(),
            "complete");
  ASSERT_EQ(web.HostValue()->value_as_SignInStatus()->state()->str(),
            "complete");
  ASSERT_EQ(ListSize(web.HostValue()->value_as_SignInStatus()->accounts()), 1u);
  EXPECT_EQ(web.HostValue()
                ->value_as_SignInStatus()
                ->accounts()
                ->Get(0)
                ->client_id()
                ->str(),
            "client-web:fixture");
  const auto status = oauth.Status(*host, "", "desktop");
  ASSERT_EQ(ListSize(status.HostValue()->value_as_SignInStatus()->accounts()),
            1u);
  EXPECT_EQ(status.HostValue()
                ->value_as_SignInStatus()
                ->accounts()
                ->Get(0)
                ->client_id()
                ->str(),
            "client-desktop");
}
TEST_F(AgentFixture, CancelledSignInDoesNotGrantOrExposeCredentials) {
  OAuthServer oauth;
  oauth.resource = "https://api.openai.com/v1";
  const auto begin = oauth.Begin(*host);
  ASSERT_TRUE(begin.Code().empty());
  const auto* value = begin.HostValue()->value_as_SignInBegin();
  EXPECT_EQ(value->url()->str().find("id_token_hint"), std::string::npos);
  const auto cancel =
      Call(*host, H::RequestPayload::SignInActionRequest, [&](Builder& b) {
        return H::CreateSignInActionRequest(
                   b, b.CreateString("desktop"),
                   b.CreateString(value->attempt_id()->str()),
                   b.CreateString("cancel"))
            .Union();
      });
  EXPECT_EQ(cancel.HostValue()->value_as_SignInStatus()->state()->str(),
            "cancelled");
  EXPECT_EQ(ListSize(cancel.HostValue()->value_as_SignInStatus()->accounts()),
            0u);
}
}  // namespace
}  // namespace tsugumi::test
