#include <algorithm>

#include "oauth_fixture.hh"
#include "tsugumi/util/sqlite.hh"

namespace tsugumi::test {
namespace {
auto Bytes(const Reply& reply) -> std::string {
  return {reply.bytes.begin(), reply.bytes.end()};
}
TEST_F(AgentFixture, HelperSettingsAreValidatedSharedAndPersisted) {
  const auto value = [](const Reply& reply, const std::string& key) {
    for (const auto* entry : *reply.HostValue()->value_as_Settings()->values())
      if (entry->key()->str() == key) return entry->value_json()->str();
    return std::string{};
  };
  EXPECT_EQ(value(SettingsGet(*host), "agent.allowUnscopedHelpers"), "false");
  EXPECT_FALSE(
      SettingsPatch(*host, "desktop", {{"agent.allowUnscopedHelpers", "1"}})
          .Code()
          .empty());
  ASSERT_TRUE(
      SettingsPatch(*host, "desktop", {{"agent.allowUnscopedHelpers", "true"}})
          .Code()
          .empty());
  auto web = ConnectHost(true);
  EXPECT_EQ(value(SettingsGet(*web), "agent.allowUnscopedHelpers"), "true");
  web.reset();
  host.reset();
  human.reset();
  external.reset();
  service.Stop();
  StartHost();
  EXPECT_EQ(value(SettingsGet(*host), "agent.allowUnscopedHelpers"), "true");
}

TEST_F(AgentFixture, RetiredPermissionSettingCannotBeReadOrWritten) {
  constexpr auto key = "agent.permissions.allowed";
  {
    sql::Db db((directory.path / "state/host-settings.sqlite").string());
    db.exec(R"(INSERT INTO settings VALUES
               ('desktop', 3, '{"agent.permissions.allowed":["network"]}'))");
  }
  const auto expect_absent = [&] {
    const auto reply = SettingsGet(*host);
    ASSERT_TRUE(reply.Code().empty());
    const auto* settings = reply.HostValue()->value_as_Settings();
    EXPECT_EQ(settings->revision(), 3u);
    for (const auto* entry : *settings->values())
      EXPECT_NE(entry->key()->str(), key);
  };
  expect_absent();
  EXPECT_EQ(SettingsPatch(*host, "desktop", {{key, "[]"}}).Code(),
            "invalid_request");
  expect_absent();
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
TEST_F(AgentFixture,
       ProviderSetAndStatusNeverReturnTheSecretAndFollowPlatformStorage) {
  const std::string key = "PRIVATE_KEY_SENTINEL";
#ifdef __APPLE__
  constexpr bool ephemeral = true;
#else
  constexpr bool ephemeral = false;
#endif
  const auto set =
      Call(*host, H::RequestPayload::ProviderSetRequest, [&](Builder& b) {
        Tsugumi::Host::ProviderInputT p;
        p.api_key = key;
        return H::CreateProviderSetRequest(
                   b, b.CreateString("desktop"), b.CreateString("anthropic"),
                   Tsugumi::Host::ProviderInput::Pack(b, &p), false, ephemeral)
            .Union();
      });
  ASSERT_TRUE(set.Code().empty());
  EXPECT_TRUE(set.HostValue()->value_as_ProviderStatus()->configured());
  EXPECT_EQ(Bytes(set).find(key), std::string::npos);
  EXPECT_EQ(Bytes(SettingsGet(*host)).find(key), std::string::npos);
  host.reset();
  human.reset();
  external.reset();
  service.Stop();
  StartHost();
  const auto status =
      Call(*host, H::RequestPayload::ProviderStatusRequest, [](Builder& b) {
        return H::CreateProviderStatusRequest(b, b.CreateString("desktop"),
                                              b.CreateString("anthropic"))
            .Union();
      });
  EXPECT_EQ(status.HostValue()->value_as_ProviderStatus()->configured(),
            !ephemeral);
  EXPECT_EQ(Bytes(status).find(key), std::string::npos);
  EXPECT_EQ(Read(directory.path / "host.log").find(key), std::string::npos);
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
    EXPECT_EQ(Read(directory.path / "host.log").find(secret),
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
  EXPECT_EQ(Read(directory.path / "host.log").find("REFRESH_SENTINEL"),
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
  EXPECT_EQ(Read(directory.path / "host.log").find("REFRESH_SENTINEL"),
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

TEST_F(AgentFixture, ANewClientCanReplaceAnAbandonedSignIn) {
  OAuthServer oauth;
  const auto begin = oauth.Begin(*host);
  ASSERT_TRUE(begin.Code().empty());
  const auto original =
      begin.HostValue()->value_as_SignInBegin()->attempt_id()->str();
  host.reset();
  host = ConnectHost();
  EXPECT_EQ(oauth.Begin(*host).Code(), "invalid_request");
  const auto replacement = oauth.Begin(*host, "desktop", true);
  ASSERT_TRUE(replacement.Code().empty());
  const auto next =
      replacement.HostValue()->value_as_SignInBegin()->attempt_id()->str();
  EXPECT_NE(next, original);
  const auto cancelled = oauth.Status(*host, original);
  EXPECT_EQ(cancelled.HostValue()->value_as_SignInStatus()->state()->str(),
            "cancelled");
  const auto pending = oauth.Status(*host, "");
  EXPECT_EQ(pending.HostValue()->value_as_SignInStatus()->attempt_id()->str(),
            next);
  EXPECT_EQ(pending.HostValue()->value_as_SignInStatus()->state()->str(),
            "pending");
}

TEST_F(AgentFixture, OAuthResultSurvivesAccountChangesAndGuardsSetup) {
  OAuthServer oauth;
  ModelServer model;
  const std::string profile = "web:fixture";
  const auto first = oauth.SignIn(*host, model.URL(), profile);
  const auto* completed = first.HostValue()->value_as_SignInStatus();
  ASSERT_EQ(completed->state()->str(), "complete");
  const auto attempt = completed->attempt_id()->str();
  const auto account = completed->client_id()->str();
  const auto before = SettingsGet(*host);
  const auto revision = before.HostValue()->value_as_Settings()->revision();
  const auto select_provider = [&] {
    return Call(*host, H::RequestPayload::SettingsUpdateRequest,
                [&](Builder& b) {
                  const auto guard = H::CreateExpectedAccount(
                      b, b.CreateString(profile), b.CreateString(account));
                  const auto change =
                      H::CreateSetting(b, b.CreateString("agent.provider"),
                                       b.CreateString(Quote("chatgpt")));
                  return H::CreateSettingsUpdateRequest(
                             b, b.CreateString("desktop"), revision,
                             b.CreateVector(std::vector{change}), guard)
                      .Union();
                });
  };
  const auto second =
      oauth.SignIn(*host, model.URL(), profile, 3600, "second-client", false);
  const auto* no_plan = second.HostValue()->value_as_SignInStatus();
  ASSERT_EQ(no_plan->state()->str(), "complete");
  EXPECT_FALSE(no_plan->plan_enabled());
  EXPECT_EQ(select_provider().Code(), "chatgpt_account_changed");
  EXPECT_EQ(SettingsGet(*host).HostValue()->value_as_Settings()->revision(),
            revision);

  const auto action = [&](const char* name, const std::string& client) {
    return Call(*host, H::RequestPayload::SignInActionRequest, [&](Builder& b) {
      return H::CreateSignInActionRequest(b, b.CreateString(profile), 0,
                                          b.CreateString(name),
                                          b.CreateString(client))
          .Union();
    });
  };
  ASSERT_TRUE(action("select", account).Code().empty());
  ASSERT_TRUE(select_provider().Code().empty());
  ASSERT_TRUE(action("select", "second-client").Code().empty());
  oauth.server.QueueHTTP("", 204);
  ASSERT_TRUE(action("signout", account).Code().empty());
  const auto result = oauth.Status(*host, attempt, profile);
  const auto* status = result.HostValue()->value_as_SignInStatus();
  EXPECT_EQ(status->state()->str(), "complete");
  EXPECT_EQ(status->client_id()->str(), account);
  EXPECT_TRUE(status->plan_enabled());
  const auto& accounts = *status->accounts();
  const auto original = std::find_if(
      accounts.begin(), accounts.end(),
      [&](const auto* row) { return row->client_id()->str() == account; });
  ASSERT_NE(original, accounts.end());
  EXPECT_FALSE((*original)->signed_in());
  EXPECT_FALSE((*original)->active());
}
}  // namespace
}  // namespace tsugumi::test
