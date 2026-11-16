// Scenarios exercise only the public Agent RPC through the shipped client.
#include <set>

#include "agent/json.hh"
#include "oauth_fixture.hh"
#include "project_fixture.hh"
#include "tsugumi/protocol/tsugumi_generated.h"

namespace tsugumi::test {
namespace {
// Match ChatResumeTests: the desktop configures a fixture without an API key
// or a custom output limit, then selects it on a human-owned conversation.
class DesktopChatFixture : public AgentProjectFixture {
protected:
  auto ConfigureDesktop(const ModelServer &model, uint32_t input_limit = 0)
      -> void {
    StartProject();
    const auto reply =
        Call(*host, H::RequestPayload::ProviderSetRequest, [&](Builder &b) {
          H::ProviderInputT config;
          config.endpoint = model.URL();
          config.model = "fixture";
          config.context_limit = input_limit;
          return H::CreateProviderSetRequest(b, b.CreateString("desktop"),
                                             b.CreateString("fixture"),
                                             H::ProviderInput::Pack(b, &config))
              .Union();
        });
    ASSERT_TRUE(reply.Code().empty());
  }

  auto DesktopConversation(const std::string &id) -> std::string {
    const auto conversation = Conversation(id, true);
    const auto reply =
        Call(*human, A::RequestPayload::ConfigureRequest, [&](Builder &b) {
          Tsugumi::Service::ProviderSelectionT selection;
          selection.profile = "desktop";
          selection.provider_id = "fixture";
          selection.model = "fixture";
          selection.account_profile = "desktop";
          return A::CreateConfigureRequest(
                     b,
                     Tsugumi::Service::ProviderSelection::Pack(b, &selection),
                     b.CreateString(conversation))
              .Union();
        });
    EXPECT_TRUE(reply.Code().empty());
    return conversation;
  }

  auto DesktopSend(const std::string &id, const std::string &text,
                   const std::string &turn) -> Reply {
    return Call(*human, A::RequestPayload::ConversationSendRequest,
                [&](Builder &b) {
                  return A::CreateConversationSendRequest(b, b.CreateString(id),
                                                          b.CreateString(turn),
                                                          b.CreateString(text))
                      .Union();
                });
  }
};

TEST_F(DesktopChatFixture, DefaultFixtureBudgetAllowsSendingAndCompacting) {
  ModelServer model;
  ConfigureDesktop(model);
  const auto conversation = DesktopConversation("desktop-chat");
  for (int turn = 1; turn <= 4; ++turn) {
    model.Queue(AnthropicText("Desktop answer " + std::to_string(turn)));
    ASSERT_TRUE(DesktopSend(conversation, "hello " + std::to_string(turn),
                            "turn-" + std::to_string(turn))
                    .Code()
                    .empty());
    const auto state = WaitUntilIdle(conversation);
    ASSERT_NE(state.find("Desktop answer " + std::to_string(turn)),
              std::string::npos)
        << Events(conversation, true).View();
  }
  model.Queue(AnthropicText("Earlier desktop messages were answered."));
  ASSERT_TRUE(Call(*human, A::RequestPayload::ConversationActionRequest,
                   [&](Builder &b) {
                     return A::CreateConversationActionRequest(
                                b, b.CreateString(conversation),
                                A::Action::Compact)
                         .Union();
                   })
                  .Code()
                  .empty());
  const auto state = WaitUntilIdle(conversation);
  EXPECT_NE(state.find("Earlier desktop messages were answered."),
            std::string::npos);
  EXPECT_EQ(JsonString(state, "/conversation/archive/0/content/0/text"),
            "hello 1");
  const auto snapshot = Get(conversation, true);
  const auto *history =
      snapshot.Value()->value_as_ConversationResult()->conversation();
  size_t visible = 0;
  for (const auto *messages : {history->archive(), history->messages()})
    if (messages)
      for (const auto *message : *messages)
        if (!message->context_summary())
          ++visible;
  EXPECT_EQ(visible, 8u);
  const auto events = Events(conversation, true).View();
  EXPECT_NE(events.find("compacted"), std::string::npos);
  EXPECT_EQ(events.find("\"kind\":\"error\""), std::string::npos) << events;
  EXPECT_EQ(model.Requests().size(), 5u);
}

TEST_F(DesktopChatFixture,
       OversizedRequestReportsContextFullAndANewChatCanSend) {
  ModelServer model;
  ConfigureDesktop(model);
  const auto conversation = DesktopConversation("desktop-oversized");
  ASSERT_TRUE(DesktopSend(conversation, std::string(100000, 'y'), "too-large")
                  .Code()
                  .empty());
  const auto state = WaitUntilIdle(conversation);
  const auto trace = Events(conversation, true);
  const auto *events = trace.Value()->value_as_EventsPage()->events();
  ASSERT_NE(events, nullptr);
  size_t errors = 0;
  for (const auto *item : *events)
    if (item->event()->kind()->str() == "error") {
      ++errors;
      ASSERT_NE(item->event()->message(), nullptr);
      EXPECT_EQ(item->event()->message()->str(),
                "the conversation no longer fits the model's context");
    }
  EXPECT_EQ(errors, 1u) << trace.View();
  EXPECT_TRUE(model.Requests().empty());
  EXPECT_EQ(JsonString(state, "/conversation/messages/0/content/0/text"),
            std::string(100000, 'y'));
  const auto fresh = DesktopConversation("desktop-fresh");
  model.Queue(AnthropicText("A fresh conversation can answer."));
  ASSERT_TRUE(DesktopSend(fresh, "hello", "fresh-turn").Code().empty());
  EXPECT_NE(WaitUntilIdle(fresh).find("A fresh conversation can answer."),
            std::string::npos)
      << Events(fresh, true).View();
  EXPECT_EQ(model.Requests().size(), 1u);
}

TEST_F(DesktopChatFixture, TooSmallExplicitBudgetRejectsEvenAShortMessage) {
  ModelServer model;
  ConfigureDesktop(model, 6000);
  const auto conversation = DesktopConversation("desktop-small-budget");
  ASSERT_TRUE(
      DesktopSend(conversation, "hello", "small-budget").Code().empty());
  WaitUntilIdle(conversation);
  EXPECT_EQ(
      JsonString(Events(conversation, true).View(), "/events/0/event/message"),
      "the conversation no longer fits the model's context");
  EXPECT_TRUE(model.Requests().empty());
}

TEST_F(AgentFixture, ConversationSnapshotReportsTheAcceptedModelAcrossHostEditsAndRestart) {
  ModelServer model;
  model.Queue(AnthropicText("done"));
  Configure(model);
  const auto conversation = Conversation("bound-model");
  ASSERT_TRUE(Send(conversation, "hello").Code().empty());
  WaitUntilIdle(conversation);
  const auto check = [&] {
    const auto reply = Get(conversation, true);
    const auto* binding = reply.Value()->value_as_ConversationResult()->model_binding();
    ASSERT_NE(binding, nullptr);
    EXPECT_EQ(binding->provider_id()->str(), "fixture");
    EXPECT_EQ(binding->model()->str(), "fixture-model");
  };
  check();
  // A host setting edit changes future selections, never the recorded model.
  const auto selected = Call(*host, H::RequestPayload::SettingsGetRequest, [](Builder& b) {
    return H::CreateSettingsGetRequest(b, b.CreateString("desktop")).Union();
  });
  ASSERT_TRUE(Call(*host, H::RequestPayload::SettingsUpdateRequest, [&](Builder& b) {
    const auto item = H::CreateSetting(b, b.CreateString("agent.model.chatgpt"), b.CreateString(Quote("gpt-6.1-terra")));
    return H::CreateSettingsUpdateRequest(b, b.CreateString("desktop"),
        selected.HostValue()->value_as_Settings()->revision(), b.CreateVector(std::vector{item})).Union();
  }).Code().empty());
  check();
  human.reset(); external.reset(); host.reset(); service.Stop(); StartHost();
  check();
}
TEST_F(AgentFixture,
       MainAndHelperPassTheirFormerStepLimitsAndStillStopOnRequest) {
  for (const bool helper : {false, true}) {
    ModelServer model;
    if (helper)
      model.Queue(AnthropicTool(
          "delegate-long", "delegate",
          R"({"tasks":[{"name":"Long review","instruction":"Keep inspecting until stopped.","read_cards":[],"write_cards":[],"run_cards":[]}]})"));
    const size_t count = helper ? 18 : 53;
    for (size_t i = 0; i < count; ++i)
      model.Queue(
          AnthropicTool("long-" + std::to_string(i), "demo_list", "{}"));
    model.Queue(AnthropicText("Must be cancelled"), true);
    Configure(model);
    const auto conversation =
        Conversation(helper ? "long-helper" : "long-main");
    ASSERT_TRUE(Send(conversation, "Continue inspecting until I stop you.")
                    .Code()
                    .empty());
    ASSERT_TRUE(model.WaitForRequests(count + (helper ? 2 : 1), 20s));
    const auto began = std::chrono::steady_clock::now();
    ASSERT_TRUE(Call(*external, A::RequestPayload::ConversationCancelRequest,
                     [&](Builder& b) {
                       return A::CreateConversationCancelRequest(
                                  b, b.CreateString(conversation))
                           .Union();
                     })
                    .Code()
                    .empty());
    const auto stopped = WaitUntilIdle(conversation);
    EXPECT_LT(std::chrono::steady_clock::now() - began, 2s);
    EXPECT_EQ(stopped.find("Must be cancelled"), std::string::npos);
    EXPECT_FALSE(JsonBool(stopped, "/busy"));
    model.Release();
  }
}
namespace R = Tsugumi::RPC;

TEST_F(AgentFixture, UnixAndWebSocketExposeTheSameAgentProtocol) {
  auto websocket = Connect(false, true);
  const auto unix_catalog =
      Call(*external, A::RequestPayload::ToolCatalogRequest, [](Builder &b) {
        return A::CreateToolCatalogRequest(b).Union();
      }).View();
  const auto websocket_catalog =
      Call(*websocket, A::RequestPayload::ToolCatalogRequest, [](Builder &b) {
        return A::CreateToolCatalogRequest(b).Union();
      }).View();
  EXPECT_EQ(unix_catalog, websocket_catalog);
  EXPECT_NE(unix_catalog.find("project_inspect"), std::string::npos);
  EXPECT_NE(unix_catalog.find("delegate"), std::string::npos);
  EXPECT_EQ(unix_catalog.find("guide_"), std::string::npos);
  // Invocation records remain project tools; version diffs are Host requests.
  EXPECT_NE(unix_catalog.find("run_latest_invocations"), std::string::npos);
  EXPECT_EQ(unix_catalog.find("version_diff"), std::string::npos);
  EXPECT_EQ(unix_catalog.find("from_commit"), std::string::npos);
}

// Inspect the actual provider requests as well as the external RPC catalog.
TEST_F(AgentFixture, ToolCatalogsKeepGuidanceExclusiveToTheGuideRole) {
  namespace J = agent::json;
  auto names = [](const auto &tools) {
    std::set<std::string> result;
    for (const auto &tool : J::Array(tools))
      result.insert(J::Text(J::Object(tool), "name"));
    return result;
  };
  ModelServer model;
  Configure(model);
  const auto main = Conversation("catalog-main");
  model.Queue(AnthropicText("Main answer"));
  ASSERT_TRUE(Send(main, "Answer briefly.").Code().empty());
  WaitUntilIdle(main);
  model.Queue(AnthropicText("Helper answer"));
  ASSERT_TRUE(
      Tool(
          main, "delegate",
          R"({"tasks":[{"name":"reader","instruction":"Answer briefly.","read_cards":[],"write_cards":[],"run_cards":[]}]})")
          .Code()
          .empty());
  const auto created = Call(
      *external, A::RequestPayload::ConversationCreateRequest, [](Builder &b) {
        return A::CreateConversationCreateRequest(
                   b, b.CreateString("catalog-guide"), 0,
                   b.CreateString("guide"))
            .Union();
      });
  ASSERT_TRUE(created.Code().empty());
  const auto guide = JsonString(created.View(), "/conversation/id");
  model.Queue(AnthropicTool("show", "guide_show",
                            R"({"say":"Choose a demo.","highlight":"demos"})"));
  model.Queue(AnthropicText("Guide answer"));
  ASSERT_TRUE(Send(guide, "Show the demos.").Code().empty());
  EXPECT_NE(WaitUntilIdle(guide).find("Guide answer"), std::string::npos);
  const auto requests = model.Requests();
  ASSERT_EQ(requests.size(), 4u);
  const auto main_tools = names(J::Get(J::Object(requests[0]), "tools"));
  auto helper_tools = names(J::Get(J::Object(requests[1]), "tools"));
  const auto guide_specs = J::Array(J::Get(J::Object(requests[2]), "tools"));
  const auto guide_tools = names(J::Encode(guide_specs));
  const auto external_catalog =
      Call(*external, A::RequestPayload::ToolCatalogRequest,
           [](Builder &b) { return A::CreateToolCatalogRequest(b).Union(); });
  EXPECT_EQ(names(external_catalog.View()), main_tools);
  EXPECT_FALSE(helper_tools.contains("delegate"));
  helper_tools.insert("delegate");
  EXPECT_EQ(helper_tools, main_tools);
  const auto helper_catalog = helper_tools;
  for (const auto *catalog : {&main_tools, &helper_catalog, &guide_tools}) {
    for (const auto *name : {"demo_list", "demo_read", "project_inspect"})
      EXPECT_TRUE(catalog->contains(name));
    std::set<std::string> guidance;
    for (const auto &name : *catalog)
      if (name.starts_with("guide_"))
        guidance.insert(name);
    EXPECT_EQ(guidance, catalog == &guide_tools
                            ? std::set<std::string>{"guide_show"}
                            : std::set<std::string>{});
  }
  EXPECT_TRUE(main_tools.contains("project_save"));
  for (const auto *name : {"delegate", "project_save", "run", "run_cancel"})
    EXPECT_FALSE(guide_tools.contains(name));
  for (const auto &raw : guide_specs) {
    const auto spec = J::Object(raw);
    if (J::Text(spec, "name") != "guide_show")
      continue;
    const auto schema = J::Object(J::Get(spec, "input_schema"));
    const auto properties = J::Object(J::Get(schema, "properties"));
    EXPECT_TRUE(properties.contains("endTour"));
    EXPECT_TRUE(properties.contains("choices"));
  }
  EXPECT_EQ(Tool(main, "guide_show", R"({"say":"Do not show this."})").Code(),
            "permission_denied");
}

TEST_F(AgentFixture, ModelDemoListIsBoundedAndTheUICatalogKeepsItsMetadata) {
  namespace J = agent::json;
  const auto bundled = std::filesystem::path(TSUGUMI_GUIDE_TEST_ROOT) / "demos";
  // Nix-installed fixtures have read-only directories. Write an owned copy
  // without inheriting their permissions.
  for (const auto& entry : std::filesystem::recursive_directory_iterator(bundled))
    if (entry.is_regular_file())
      Write(directory.path / "demos" / entry.path().lexically_relative(bundled),
            Read(entry.path()));
  const auto conversation = Conversation("compact-demos");
  const auto reply = Tool(conversation, "demo_list", "{}");
  ASSERT_TRUE(reply.Code().empty());
  const auto text = reply.Value()->value_as_ToolResult()->text()->str();
  // The full bundled file-path list was about 60 KB. Keep room for new demos
  // while catching a return to file enumeration or embedded content.
  EXPECT_LT(text.size(), 20u * 1024u);
  const auto demos = J::Array(text);
  size_t expected_count = 0;
  for (const auto *language : {"ja", "en"}) {
    const auto index = J::Object(Read(bundled / language / "index.json"));
    const auto entries = J::Array(J::Get(index, "demos"));
    const auto ui =
        Call(*host, H::RequestPayload::HostDemoListRequest, [&](Builder &b) {
          return H::CreateHostDemoListRequest(b, b.CreateString(language))
              .Union();
        });
    ASSERT_TRUE(ui.Code().empty());
    const auto ui_entries = J::Array(ui.View());
    ASSERT_EQ(ui_entries.size(), entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
      const auto source = J::Object(entries[i]);
      const auto demo = J::Object(demos.at(expected_count++));
      EXPECT_EQ(demo.size(), 3u);
      EXPECT_EQ(J::Text(demo, "path"),
                std::string(language) + "/" + J::Text(source, "path"));
      EXPECT_EQ(J::Text(demo, "name"), J::Text(source, "title"));
      EXPECT_EQ(J::Text(demo, "description"), J::Text(source, "description"));
      EXPECT_EQ(J::Text(demo, "description").find_first_of("\r\n"),
                std::string::npos);
      const auto ui_demo = J::Object(ui_entries[i]);
      for (const auto* key : {"id", "title", "description", "field"})
        EXPECT_EQ(J::Text(ui_demo, key), J::Text(source, key));
      const auto ui_tags = J::Array(J::Get(ui_demo, "tags"));
      const auto source_tags = J::Array(J::Get(source, "tags"));
      ASSERT_EQ(ui_tags.size(), source_tags.size());
      for (size_t tag = 0; tag < source_tags.size(); ++tag)
        EXPECT_EQ(J::String(ui_tags[tag]), J::String(source_tags[tag]));
    }
  }
  EXPECT_GT(expected_count, 0u);
  EXPECT_EQ(demos.size(), expected_count);
  for (const auto *path : {"en/index.json", "en/report/tsugumi.json",
                           "en/report/definitions/generate/src/main.py"}) {
    const auto content = Tool(conversation, "demo_read",
                              J::Encode(J::Members{{"path", J::Quote(path)}}));
    ASSERT_TRUE(content.Code().empty());
    EXPECT_EQ(content.Value()->value_as_ToolResult()->text()->str(),
              Read(bundled / path));
  }
  ModelServer model;
  Configure(model);
  model.Queue(AnthropicTool("list-demos", "demo_list", "{}"));
  model.Queue(AnthropicText("Listed demos."));
  ASSERT_TRUE(Send(conversation, "List the bundled demos.").Code().empty());
  WaitUntilIdle(conversation);
  const auto requests = model.Requests();
  ASSERT_EQ(requests.size(), 2u);
  EXPECT_EQ(JsonString(requests[1], "/messages/2/content/0/content"), text);
}

TEST_F(AgentFixture, ProjectProtocolCannotBeUsedOnTheAgentSocket) {
  client::Options options;
  options.protocol = client::Protocol::Project;
  options.endpoint = "unix:" + JsonString(ready, "/unix_socket");
  client::Connection wrong(options);
  ASSERT_FALSE(wrong.Start().get());
  auto result =
      wrong
          .Call(
              [](uint64_t id) {
                Builder b;
                const auto hello = R::CreateSessionHelloRequest(
                    b, 1, b.CreateString("wrong-protocol"));
                const auto request = R::CreateRequest(
                    b, R::RequestPayload::SessionHelloRequest, hello.Union());
                R::FinishMessageBuffer(
                    b, R::CreateMessage(b, id, R::Payload::Request,
                                        request.Union()));
                return client::Bytes(b.GetBufferPointer(),
                                     b.GetBufferPointer() + b.GetSize());
              },
              2s)
          .get();
  EXPECT_TRUE(result.error);
  EXPECT_TRUE(service.Alive());
  EXPECT_TRUE(
      Call(*external, A::RequestPayload::ToolCatalogRequest,
           [](Builder& b) { return A::CreateToolCatalogRequest(b).Union(); })
          .Code()
          .empty());
}

TEST_F(AgentFixture, ExternalAgentsCannotConfigureConversationProviders) {
  const auto result =
      Call(*external, A::RequestPayload::ConfigureRequest, [](Builder& b) {
        Tsugumi::Service::ProviderSelectionT config;
        config.profile = "desktop";
        config.model = "injected";
        const auto p = Tsugumi::Service::ProviderSelection::Pack(b, &config);
        return A::CreateConfigureRequest(b, p, b.CreateString("untrusted"))
            .Union();
      });
  EXPECT_EQ(result.Code(), "permission_denied");
}

TEST_F(AgentFixture, InvalidRequestIdsAndActionsLeaveTheConversationUnchanged) {
  ModelServer model;
  Configure(model);
  const auto conversation = Conversation("invalid-request-values");
  const auto initial = Get(conversation).View();
  for (const auto& id : {std::string{}, std::string(257, 'x')}) {
    SCOPED_TRACE(id.empty() ? "empty ID" : "oversized ID");
    EXPECT_EQ(Send(conversation, "This must not reach the model.", id).Code(),
              "invalid_request");
    const auto tool =
        Call(*external, A::RequestPayload::ToolCallRequest, [&](Builder& b) {
          return A::CreateToolCallRequest(
                     b, b.CreateString(conversation), 0,
                     b.CreateString("demo_read"),
                     b.CreateString(R"({"path":"ja/example.json"})"),
                     b.CreateString(id))
              .Union();
        });
    EXPECT_EQ(tool.Code(), "invalid_request");
  }
  const auto action = Call(
      *external, A::RequestPayload::ConversationActionRequest, [&](Builder& b) {
        return A::CreateConversationActionRequest(
                   b, b.CreateString(conversation), static_cast<A::Action>(255))
            .Union();
      });
  EXPECT_EQ(action.Code(), "invalid_request");
  EXPECT_FALSE(model.WaitForRequests(1, 100ms));
  EXPECT_EQ(Get(conversation).View(), initial);
}

TEST_F(AgentFixture, ExternalAgentsCannotReadOrReplaceHumanConversations) {
  const auto conversation = Conversation("human-owned", true);
  EXPECT_EQ(Get(conversation).Code(), "permission_denied");
  EXPECT_EQ(Events(conversation).Code(), "permission_denied");
  EXPECT_EQ(Send(conversation, "Replace the human's request.").Code(),
            "permission_denied");
  EXPECT_EQ(Call(*external, A::RequestPayload::ConversationCreateRequest,
                 [&](Builder& b) {
                   return A::CreateConversationCreateRequest(
                              b, b.CreateString(conversation))
                       .Union();
                 })
                .Code(),
            "permission_denied");
  const auto visible =
      Call(*external, A::RequestPayload::ConversationListRequest,
           [](Builder &b) {
             return A::CreateConversationListRequest(b).Union();
           })
          .View();
  EXPECT_EQ(visible.find(conversation), std::string::npos);
  EXPECT_TRUE(Get(conversation, true).Code().empty());
}

TEST_F(AgentFixture,
       ChatGPTPlanLimitsStopWithoutRetryingAndUseThePlanWireFormat) {
  ModelServer model;
  model.QueueHTTP(
      R"({"error":{"code":"subscription_sharing_usage_limit_exceeded","message":"usage limit reached"}})",
      429);
  model.Queue(
      "data: {\"type\":\"response.failed\",\"response\":{\"error\":{"
      "\"code\":\"subscription_sharing_usage_limit_exceeded\","
      "\"message\":\"usage limit reached\"}}}\n\n");
  OAuthServer oauth;
  ASSERT_EQ(oauth.SignIn(*host, model.URL())
                .HostValue()
                ->value_as_SignInStatus()
                ->state()
                ->str(),
            "complete");
  ASSERT_TRUE(
      SettingsPatch(*host, "desktop", {{"agent.provider", Quote("chatgpt")}})
          .Code()
          .empty());
  for (int turn = 0; turn < 2; ++turn) {
    const auto conversation =
        Conversation("plan-limit-" + std::to_string(turn));
    ASSERT_TRUE(Send(conversation, "Respond on the plan.").Code().empty());
    ASSERT_TRUE(model.WaitForRequests(turn + 1));
    WaitUntilIdle(conversation);
    const auto events = Events(conversation).View();
    EXPECT_NE(events.find("ChatGPT usage limit reached"), std::string::npos);
    EXPECT_EQ(events.find("retrying"), std::string::npos);
    EXPECT_EQ(model.Requests().size(), static_cast<size_t>(turn + 1));
  }
  const auto request = model.Requests()[0];
  EXPECT_FALSE(JsonBool(request, "/store"));
  EXPECT_TRUE(JsonBool(request, "/stream"));
  EXPECT_EQ(JsonString(request, "/tools/0/type"), "namespace");
  EXPECT_THROW(JsonUInt(request, "/max_output_tokens"), std::runtime_error);
}

TEST_F(AgentFixture, BundledDemosAreReadableButSymlinksAndTraversalAreNot) {
  const auto conversation = Conversation("demo-references");
  const auto listed = Tool(conversation, "demo_list", "{}");
  ASSERT_TRUE(listed.Code().empty());
  EXPECT_EQ(listed.Value()->value_as_ToolResult()->text()->str(),
            R"([{"description":"A bundled demo.","name":"Example","path":"ja/example"}])");
  const auto tool = Tool(conversation, "demo_read", R"({"path":"ja/example.json"})");
  ASSERT_TRUE(tool.Code().empty());
  EXPECT_NE(tool.Value()->value_as_ToolResult()->text()->str().find("BUNDLED_DEMO"), std::string::npos);
  const auto paths = Call(*host, H::RequestPayload::DemoListRequest, [](Builder& b) {
    return H::CreateDemoListRequest(b).Union();
  });
  ASSERT_NE(paths.HostValue()->value_as_DemoPaths(), nullptr);
  EXPECT_NE(paths.View().find("ja/example.json"), std::string::npos);
  const auto good =
      Call(*host, H::RequestPayload::DemoReadRequest, [](Builder& b) {
        return H::CreateDemoReadRequest(b, b.CreateString("ja/example.json"))
            .Union();
      });
  EXPECT_NE(good.View().find("BUNDLED_DEMO"), std::string::npos);
  for (const auto* path : {"../private.json", "escape.json", "/private.json"}) {
    SCOPED_TRACE(path);
    const auto reply =
        Call(*host, H::RequestPayload::DemoReadRequest, [&](Builder& b) {
          return H::CreateDemoReadRequest(b, b.CreateString(path)).Union();
        });
    EXPECT_FALSE(reply.Code().empty());
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(reply.bytes.data()),
                          reply.bytes.size())
                  .find("PRIVATE_OUTSIDE_DEMO"),
              std::string::npos);
  }
}

TEST_F(AgentFixture, ConversationContinuesAfterTheClientDisconnects) {
  ModelServer model;
  model.Queue(AnthropicText("The core kept working."), true, true);
  Configure(model);
  const auto conversation = Conversation("detached");
  ASSERT_TRUE(Send(conversation, "Continue while this connection closes.")
                  .Code()
                  .empty());
  ASSERT_TRUE(model.WaitForRequests(1));
  external.reset();
  model.Release();
  external = Connect(false, true);
  const auto state = WaitUntilIdle(conversation);
  EXPECT_NE(state.find("The core kept working."), std::string::npos);
  EXPECT_NE(Events(conversation).View().find("finished"), std::string::npos);
}

TEST_F(AgentFixture, RestartAndObservationLeaveTheTurnInterruptedUntilResume) {
  ModelServer model;
  model.Queue(AnthropicText("Unfinished response"), true);
  model.Queue(AnthropicText("Explicitly resumed"));
  Configure(model);
  const auto conversation = Conversation("explicit-resume");
  ASSERT_TRUE(Send(conversation, "Start a response").Code().empty());
  ASSERT_TRUE(model.WaitForRequests(1));
  host.reset();
  external.reset();
  human.reset();
  service.Stop(true);
  StartHost();
  model.Release();
  EXPECT_FALSE(model.WaitForRequests(2, 150ms));
  // Create is also the public operation used when opening an existing chat.
  EXPECT_EQ(Conversation(conversation), conversation);
  auto observer = Connect(false, true);
  for (int i = 0; i < 3; ++i) {
    const auto snapshot = Get(conversation).View();
    EXPECT_FALSE(JsonBool(snapshot, "/busy"));
    EXPECT_TRUE(JsonBool(snapshot, "/resumable"));
    EXPECT_EQ(JsonString(snapshot, "/interrupted_operation/kind"), "model");
    EXPECT_TRUE(Call(*observer, A::RequestPayload::EventsRequest,
                     [&](Builder &b) {
                       return A::CreateEventsRequest(
                                  b, b.CreateString(conversation))
                           .Union();
                     })
                    .Code()
                    .empty());
  }
  EXPECT_FALSE(model.WaitForRequests(2, 150ms));
  ASSERT_TRUE(Call(*external, A::RequestPayload::ConversationActionRequest,
                   [&](Builder &b) {
                     return A::CreateConversationActionRequest(
                                b, b.CreateString(conversation),
                                A::Action::Resume)
                         .Union();
                   })
                  .Code()
                  .empty());
  EXPECT_NE(WaitUntilIdle(conversation).find("Explicitly resumed"),
            std::string::npos);
  EXPECT_EQ(model.Requests().size(), 2u);
  EXPECT_FALSE(JsonBool(Get(conversation).View(), "/resumable"));
}

TEST_F(AgentProjectFixture,
       InterruptedToolIsRecordedAndANewMessageCanReplaceResume) {
  ModelServer model;
  model.Queue(AnthropicTool("pending-read", "project_inspect", "{}"));
  model.Queue(AnthropicText("Followed the new message"));
  Configure(model);
  StartProject();
  const auto conversation = Conversation("interrupted-operation");
  ASSERT_TRUE(Send(conversation, "Inspect the project").Code().empty());
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (Events(conversation).View().find("permission") == std::string::npos &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(10ms);
  ASSERT_NE(Events(conversation).View().find("permission"), std::string::npos);
  host.reset();
  external.reset();
  human.reset();
  service.Stop(true);
  StartHost();
  const auto snapshot = Get(conversation).View();
  EXPECT_TRUE(JsonBool(snapshot, "/resumable"));
  EXPECT_EQ(JsonString(snapshot, "/interrupted_operation/kind"), "tool");
  EXPECT_EQ(JsonString(snapshot, "/interrupted_operation/name"),
            "project_inspect");
  EXPECT_EQ(JsonString(snapshot, "/interrupted_operation/id"), "pending-read");
  EXPECT_EQ(JsonString(snapshot, "/interrupted_operation/arguments_json"),
            "{}");
  EXPECT_FALSE(model.WaitForRequests(2, 150ms));
  Grant(conversation);
  EXPECT_FALSE(model.WaitForRequests(2, 150ms));
  // A new message needs a new idempotency key; the fixture defaults to turn-1.
  // Both UIs encode empty selection vectors, rather than omitting them.
  const auto sent = Call(
      *external, A::RequestPayload::ConversationSendRequest, [&](Builder &b) {
        const auto empty = b.CreateVector(
            std::vector<flatbuffers::Offset<flatbuffers::String>>{});
        return A::CreateConversationSendRequest(
                   b, b.CreateString(conversation), b.CreateString("turn-2"),
                   b.CreateString("Do something else instead"), empty, empty)
            .Union();
      });
  ASSERT_TRUE(sent.Code().empty()) << sent.Code();
  EXPECT_NE(WaitUntilIdle(conversation).find("Followed the new message"),
            std::string::npos);
  ASSERT_TRUE(model.WaitForRequests(2));
  ASSERT_EQ(model.Requests().size(), 2u);
  EXPECT_EQ(JsonString(model.Requests()[1], "/messages/2/content/0/type"),
            "tool_result");
  EXPECT_EQ(JsonString(model.Requests()[1], "/messages/2/content/0/tool_use_id"),
            "pending-read");
  EXPECT_TRUE(JsonBool(model.Requests()[1], "/messages/2/content/0/is_error"));
  EXPECT_EQ(JsonString(model.Requests()[1], "/messages/3/content/0/text"),
            "Do something else instead");
  EXPECT_NE(model.Requests()[1].find("Do something else instead"),
            std::string::npos);
  EXPECT_NE(model.Requests()[1].find("The tool was not replayed"),
            std::string::npos);
  const auto recorded = Get(conversation).View();
  EXPECT_EQ(JsonString(recorded, "/conversation/messages/3/display"),
            "Do something else instead");
  EXPECT_EQ(JsonString(recorded, "/conversation/messages/3/content/0/text"),
            "Do something else instead");
}

TEST_F(AgentFixture, DisplaySurvivesPreparationAndTheCompletedSnapshot) {
  ModelServer model;
  Configure(model);
  const auto conversation = Conversation("display-preserved");
  model.Queue(AnthropicText("Earlier answer"));
  ASSERT_TRUE(Send(conversation, "Earlier request").Code().empty());
  WaitUntilIdle(conversation);
  const auto before = Get(conversation);
  const auto archive_size = ListSize(
      before.Value()->value_as_ConversationResult()->conversation()->archive());
  model.Queue(AnthropicText("Finished"), true);
  ASSERT_TRUE(
      Call(*external, A::RequestPayload::ConversationSendRequest,
           [&](Builder &b) {
             A::ConversationSendRequestT request;
             request.conversation_id = conversation;
             request.turn_id = "display-turn";
             request.text = "Use the demo skill";
             request.display = "A short request";
             request.skill_names = {"demo"};
             return A::ConversationSendRequest::Pack(b, &request).Union();
           })
          .Code()
          .empty());
  ASSERT_TRUE(model.WaitForRequests(2));
  const auto preparing = Get(conversation).View();
  EXPECT_TRUE(JsonBool(preparing, "/busy"));
  EXPECT_EQ(JsonString(preparing, "/conversation/messages/2/display"),
            "A short request");
  EXPECT_NE(JsonString(preparing, "/conversation/messages/2/content/0/text")
                .find("[Skill demo]"),
            std::string::npos);
  const auto during = Get(conversation);
  EXPECT_EQ(ListSize(during.Value()
                         ->value_as_ConversationResult()
                         ->conversation()
                         ->archive()),
            archive_size);
  EXPECT_EQ(model.Requests()[1].find("A short request"), std::string::npos);
  model.Release();
  const auto completed = WaitUntilIdle(conversation);
  EXPECT_EQ(JsonString(completed, "/conversation/messages/0/content/0/text"),
            "Earlier request");
  EXPECT_EQ(JsonString(completed, "/conversation/messages/1/content/0/text"),
            "Earlier answer");
  EXPECT_EQ(JsonString(completed, "/conversation/messages/2/display"),
            "A short request");
  EXPECT_EQ(JsonString(completed, "/conversation/messages/3/content/0/text"),
            "Finished");
  const auto after = Get(conversation);
  const auto* presented =
      after.Value()->value_as_ConversationResult()->conversation();
  EXPECT_EQ(ListSize(presented->archive()), archive_size);
  EXPECT_EQ(ListSize(presented->messages()), 4u);
}

TEST_F(AgentFixture, PublicCancellationStopsAnInFlightModelRequest) {
  ModelServer model;
  model.Queue(AnthropicText("This must never become a completed answer."),
              true);
  Configure(model);
  const auto conversation = Conversation("cancel");
  ASSERT_TRUE(Send(conversation, "Wait for cancellation.").Code().empty());
  ASSERT_TRUE(model.WaitForRequests(1));
  ASSERT_TRUE(Call(*external, A::RequestPayload::ConversationCancelRequest,
                   [&](Builder& b) {
                     return A::CreateConversationCancelRequest(
                                b, b.CreateString(conversation))
                         .Union();
                   })
                  .Code()
                  .empty());
  const auto state = WaitUntilIdle(conversation);
  EXPECT_EQ(state.find("This must never become a completed answer."),
            std::string::npos);
  EXPECT_FALSE(JsonBool(state, "/resumable"));
  EXPECT_TRUE(JsonBool(state, "/continuable"));
  EXPECT_NE(Events(conversation).View().find("cancelled"), std::string::npos);
}

TEST_F(AgentFixture, OpenAIResponsesUsesTheSameCoreConversationService) {
  ModelServer model;
  model.Queue(ResponsesText("Responses also run inside core."), false, true);
  Configure(model, "openai");
  const auto conversation = Conversation("responses");
  ASSERT_TRUE(Send(conversation, "Respond through the configured provider.")
                  .Code()
                  .empty());
  ASSERT_TRUE(model.WaitForRequests(1));
  EXPECT_NE(WaitUntilIdle(conversation).find("Responses also run inside core."),
            std::string::npos);
  const auto requests = model.Requests();
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_FALSE(JsonBool(requests[0], "/store"));
  EXPECT_TRUE(JsonBool(requests[0], "/stream"));
}

TEST_F(AgentFixture, AnInterruptedModelStreamNeverExecutesItsToolCall) {
  ModelServer model;
  auto incomplete =
      AnthropicTool("unfinished", "demo_read", R"({"path":"ja/example.json"})");
  incomplete.erase(incomplete.rfind("data:"));
  model.Queue(incomplete);
  model.Queue(AnthropicText("Recovered from the interrupted response."));
  Configure(model);
  const auto conversation = Conversation("interrupted-stream");
  ASSERT_TRUE(
      Send(conversation, "Use complete model responses only.").Code().empty());
  ASSERT_TRUE(model.WaitForRequests(2));
  EXPECT_NE(WaitUntilIdle(conversation)
                .find("Recovered from the interrupted response."),
            std::string::npos);
  const auto events = Events(conversation).View();
  EXPECT_EQ(events.find("\"kind\":\"toolStarted\""), std::string::npos);
  EXPECT_NE(events.find("retrying"), std::string::npos);
}

TEST_F(AgentFixture, OutputLimitDoesNotExecuteEvenAnOtherwiseCompleteToolCall) {
  ModelServer model;
  auto limited =
      AnthropicTool("cut-output", "demo_read", R"({"path":"ja/example.json"})");
  const auto reason = limited.find("\"stop_reason\":\"tool_use\"");
  ASSERT_NE(reason, std::string::npos);
  limited.replace(reason, std::string("\"stop_reason\":\"tool_use\"").size(),
                  "\"stop_reason\":\"max_tokens\"");
  model.Queue(limited);
  model.Queue(AnthropicText("The oversized response was replaced safely."));
  Configure(model);
  const auto conversation = Conversation("cut-output");
  ASSERT_TRUE(Send(conversation, "Handle the output limit.").Code().empty());
  ASSERT_TRUE(model.WaitForRequests(2));
  EXPECT_NE(WaitUntilIdle(conversation)
                .find("The oversized response was replaced safely."),
            std::string::npos);
  EXPECT_EQ(Events(conversation).View().find("\"kind\":\"toolStarted\""),
            std::string::npos);
  EXPECT_EQ(model.Requests()[1].find("\"type\":\"tool_use\""),
            std::string::npos);
}

TEST_F(AgentFixture, InvalidToolArgumentsReturnAnErrorWithoutReadingTheDemo) {
  ModelServer model;
  model.Queue(AnthropicTool("broken-object", "demo_read", "{\"path\":"));
  model.Queue(AnthropicTool("wrong-shape", "demo_read", "[{}]"));
  model.Queue(AnthropicTool("fixed-object", "demo_read",
                            R"({"path":"ja/example.json"})"));
  model.Queue(AnthropicText("The corrected call read the demo."));
  Configure(model);
  const auto conversation = Conversation("invalid-arguments");
  ASSERT_TRUE(Send(conversation, "Read the demo.").Code().empty());
  ASSERT_TRUE(model.WaitForRequests(4));
  EXPECT_NE(
      WaitUntilIdle(conversation).find("The corrected call read the demo."),
      std::string::npos);
  const auto requests = model.Requests();
  EXPECT_NE(requests[1].find("Nothing was executed"), std::string::npos);
  EXPECT_EQ(requests[1].find("BUNDLED_DEMO"), std::string::npos);
  EXPECT_EQ(requests[2].find("BUNDLED_DEMO"), std::string::npos);
  EXPECT_NE(requests[3].find("BUNDLED_DEMO"), std::string::npos);
}

TEST_F(AgentFixture, AnEmptyModelReplyDoesNotPreventTheNextConversationTurn) {
  ModelServer model;
  model.Queue(
      "data: {\"type\":\"message_start\",\"message\":{}}\n\n"
      "data: "
      "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"refusal\"}}\n\n"
      "data: {\"type\":\"message_stop\"}\n\n");
  model.Queue(AnthropicText("A later request can still be answered."));
  Configure(model);
  const auto conversation = Conversation("empty-response");
  ASSERT_TRUE(
      Send(conversation, "A request without a model answer.").Code().empty());
  ASSERT_TRUE(model.WaitForRequests(1));
  WaitUntilIdle(conversation);
  EXPECT_NE(Events(conversation).View().find("empty reply"), std::string::npos);
  ASSERT_TRUE(
      Send(conversation, "Try another request.", "turn-2").Code().empty());
  ASSERT_TRUE(model.WaitForRequests(2));
  EXPECT_NE(WaitUntilIdle(conversation)
                .find("A later request can still be answered."),
            std::string::npos);
  EXPECT_EQ(model.Requests()[1].find("\"content\":[]"), std::string::npos);
}

TEST_F(AgentFixture, LargeSerializedConversationUsesReportedTokensAcrossTurns) {
  ModelServer model;
  std::string answer;
  for (int i = 0; i < 8000; ++i) answer += "\"\\\n";
  for (int i = 0; i < 3; ++i) model.Queue(AnthropicText(answer, 1000));
  model.Queue(AnthropicText("The conversation continued without compaction.", 2000));
  Configure(model);
  const auto conversation = Conversation("token-context");
  for (int turn = 1; turn <= 3; ++turn) {
    ASSERT_TRUE(Send(conversation, "Continue.", "turn-" + std::to_string(turn))
                    .Code().empty());
    WaitUntilIdle(conversation);
  }
  ASSERT_GT(Get(conversation).View().size(), 180000u);
  ASSERT_TRUE(Send(conversation, "Keep working.", "turn-4").Code().empty());
  EXPECT_NE(WaitUntilIdle(conversation).find("continued without compaction"),
            std::string::npos);
  EXPECT_EQ(model.Requests().size(), 4u);
  EXPECT_EQ(Events(conversation).View().find("compacted"), std::string::npos);
}

TEST_F(AgentFixture,
       AutomaticCompactionKeepsTheArchiveAndContinuesTheConversation) {
  ModelServer model;
  const auto earlier = "FIRST_LONG_ANSWER " + std::string(1500, 'a');
  model.Queue(AnthropicText(earlier));
  model.Queue(AnthropicText("SECOND_LONG_ANSWER " + std::string(1500, 'b')));
  model.Queue(
      AnthropicText("THIRD_LONG_ANSWER " + std::string(1500, 'c'), 109000));
  model.Queue(
      AnthropicText("Earlier decisions were retained in a brief summary."));
  model.Queue(AnthropicText("The fourth turn continued after compaction."));
  Configure(model);
  const auto conversation = Conversation("automatic-compaction");
  for (int turn = 1; turn <= 3; ++turn) {
    ASSERT_TRUE(
        Send(conversation, "Continue the task.", "turn-" + std::to_string(turn))
            .Code()
            .empty());
    ASSERT_TRUE(model.WaitForRequests(turn));
    WaitUntilIdle(conversation);
  }
  // The real system prompt and tool catalog must fit too. Measured usage
  // triggers compaction; a 4K budget cannot even hold those fixed inputs.
  Configure(model, "fixture", 110000);
  ASSERT_TRUE(Send(conversation, "Keep working.", "turn-4").Code().empty());
  ASSERT_TRUE(model.WaitForRequests(5));
  const auto state = WaitUntilIdle(conversation);
  EXPECT_NE(state.find("The fourth turn continued after compaction."),
            std::string::npos);
  EXPECT_NE(JsonString(state, "/conversation/summary")
                .find("Earlier decisions were retained"),
            std::string::npos);
  EXPECT_EQ(JsonString(state, "/conversation/archive/1/content/0/text"),
            earlier);
  EXPECT_EQ(Events(conversation)
                .View()
                .find("Earlier decisions were retained in a brief summary."),
            std::string::npos);
  int retained_answers = 0;
  for (const auto* field : {"archive", "messages"}) {
    for (size_t index = 0;; ++index) {
      try {
        const auto text =
            JsonString(state, "/conversation/" + std::string(field) + '/' +
                                  std::to_string(index) + "/content/0/text");
        if (text.starts_with("THIRD_LONG_ANSWER")) ++retained_answers;
      } catch (const std::runtime_error&) {
        break;
      }
    }
  }
  EXPECT_EQ(retained_answers, 1);
  const auto requests = model.Requests();
  EXPECT_NE(requests[3].find("Create a short handoff for the next model continuation"),
            std::string::npos);
  EXPECT_EQ(requests[4].find(earlier), std::string::npos);
  EXPECT_NE(requests[4].find("Earlier decisions were retained"),
            std::string::npos);
  EXPECT_NE(Events(conversation).View().find("compacted"), std::string::npos);
  const auto found =
      Call(*external, A::RequestPayload::ConversationSearchRequest,
           [](Builder& b) {
             return A::CreateConversationSearchRequest(
                        b, 0, b.CreateString("first_long_answer"))
                 .Union();
           })
          .View();
  EXPECT_EQ(JsonString(found, "/0/session"), conversation);
  const auto snippet = JsonString(found, "/0/snippet");
  EXPECT_NE(snippet.find("FIRST_LONG_ANSWER"), std::string::npos);
  EXPECT_LT(snippet.size(), earlier.size());
  EXPECT_EQ(model.Requests().size(), 5u);
}

TEST_F(AgentFixture, AHelpersCompactionKeepsItsEarlierOperationsReadable) {
  ModelServer model;
  model.Queue(AnthropicTool(
      "compacting-helper", "delegate",
      R"({"tasks":[{"name":"long","instruction":"Inspect the demos three times.","read_cards":[],"write_cards":[],"run_cards":[]}]})"));
  // Report usage near the real input budget after the second and third
  // operations. Token usage, rather than serialized bytes, triggers compaction.
  const auto full_context_tool = [](const std::string& id) {
    auto stream = AnthropicTool(id, "demo_list", "{}");
    const std::string usage = "\"input_tokens\":12";
    stream.replace(stream.find(usage), usage.size(),
                   "\"input_tokens\":130000");
    return stream;
  };
  model.Queue(AnthropicTool("inspect-0", "demo_list", "{}"));
  model.Queue(full_context_tool("inspect-1"));
  model.Queue(AnthropicText("FIRST_HELPER_SUMMARY"));
  model.Queue(full_context_tool("inspect-2"));
  model.Queue(AnthropicText("SECOND_HELPER_SUMMARY"));
  model.Queue(AnthropicText("The helper inspected three times."));
  model.Queue(AnthropicText("The parent received the helper's result."));
  Configure(model, "fixture", 110000);
  const auto conversation = Conversation("compacting-helper");
  ASSERT_TRUE(Send(conversation, "Delegate the inspection.").Code().empty());
  ASSERT_TRUE(model.WaitForRequests(8, 20s));
  const auto state = WaitUntilIdle(conversation);
  ASSERT_EQ(model.Requests().size(), 8u) << state;
  ASSERT_NE(model.Requests()[5].find("Create a short handoff for the next model continuation"),
            std::string::npos);
  // The model went on without the first call ...
  EXPECT_EQ(model.Requests()[6].find("inspect-0"), std::string::npos);
  EXPECT_EQ(JsonString(state, "/conversation/helpers/0/state"), "completed");
  // ... and a person still reads every operation of the helper, in order,
  // from the messages the chat's rows are built from.
  const auto calls = [](const std::string& view) {
    std::vector<std::string> ids;
    for (size_t index = 0;; ++index) {
      const auto message =
          "/conversation/helpers/0/messages/" + std::to_string(index);
      try {
        JsonString(view, message + "/role");
      } catch (const std::runtime_error&) {
        break;
      }
      if (JsonBool(view, message + "/contextSummary")) continue;
      try {
        ids.push_back(JsonString(view, message + "/content/0/id"));
      } catch (const std::runtime_error&) {
        // A text or tool result block has no call ID.
      }
    }
    return ids;
  };
  const std::vector<std::string> expected{"inspect-0", "inspect-1",
                                          "inspect-2"};
  EXPECT_EQ(calls(state), expected) << state;
  EXPECT_EQ(JsonString(state, "/conversation/helpers/0/messages/0/content/0/text")
                .find("Inspect the demos three times."),
            0u);
  EXPECT_EQ(JsonString(state,
                       "/conversation/helpers/0/messages/2/content/0/tool_use_id"),
            "inspect-0");
  // A restart reads the same messages from what was saved.
  external.reset();
  human.reset();
  service.Stop();
  StartHost();
  const auto restored = Get(conversation);
  EXPECT_EQ(calls(restored.View()), expected);
  const auto* history =
      restored.Value()->value_as_ConversationResult()->conversation();
  // Only the helper compacted. Ordinary parent checkpoints must not archive
  // copies of its delegate call after provider JSON was scrubbed/reordered.
  EXPECT_EQ(ListSize(history->archive()), 0u) << restored.View();
  ASSERT_NE(history->rows(), nullptr);
  ASSERT_FALSE(history->rows()->empty());
  const A::ChatRow* step = nullptr;
  size_t delegate_steps = 0;
  for (const auto* row : *history->rows()) {
    if (row->kind()->str() != "step" || !row->children()) continue;
    for (const auto* child : *row->children()) {
      if (child->kind()->str() != "tool" || child->name()->str() != "delegate")
        continue;
      step = row;
      ++delegate_steps;
    }
  }
  ASSERT_EQ(delegate_steps, 1u) << restored.View();
  ASSERT_NE(step, nullptr);
  const A::ChatRow* helper = nullptr;
  size_t helper_count = 0;
  for (const auto* child : *step->children()) {
    if (child->kind()->str() != "helper") continue;
    helper = child;
    ++helper_count;
  }
  ASSERT_EQ(helper_count, 1u) << restored.View();
  ASSERT_NE(helper, nullptr);
  EXPECT_EQ(helper->name()->str(), "long");
  EXPECT_EQ(helper->text()->str(), "The helper inspected three times.");
  EXPECT_EQ(Events(conversation).View().find("FIRST_HELPER_SUMMARY"),
            std::string::npos);
  EXPECT_EQ(Events(conversation).View().find("SECOND_HELPER_SUMMARY"),
            std::string::npos);
  EXPECT_NE(model.Requests()[7].find("The helper inspected three times."),
            std::string::npos);
}

TEST_F(AgentFixture, MainAgentMaySpawnMoreThanEightTasksWithAnyDepth) {
  ModelServer model;
  Configure(model);
  const auto conversation = Conversation("unlimited-main");
  std::string tasks = "{\"tasks\":[";
  for (int i = 0; i < 9; ++i) {
    if (i) tasks += ',';
    tasks += "{\"name\":\"child-" + std::to_string(i) +
             "\",\"instruction\":\"Return.\",\"read_cards\":[],\"write_cards\":"
             "[],\"run_cards\":[],\"depth\":100,\"count\":100}";
    model.Queue(AnthropicText("Done."));
  }
  tasks += "]}";
  ASSERT_TRUE(
      Tool(conversation, "delegate", tasks, "unlimited").Code().empty());
  EXPECT_EQ(model.Requests().size(), 9u);
  EXPECT_EQ(ListSize(Get(conversation)
                         .Value()
                         ->value_as_ConversationResult()
                         ->conversation()
                         ->helpers()),
            9u);
}

TEST_F(AgentFixture,
       CancellingAHelperPreservesItsTraceWithoutReplayingItOnResume) {
  ModelServer model;
  model.Queue(AnthropicTool(
      "cancelled-delegation", "delegate",
      R"({"tasks":[{"name":"review","instruction":"Prepare a bounded review.","read_cards":[],"write_cards":[],"run_cards":[]}]})"));
  model.Queue(AnthropicText("This cancelled helper answer must not be saved."),
              true);
  model.Queue(AnthropicText("The parent acknowledged the interrupted helper."));
  Configure(model);
  const auto conversation = Conversation("cancelled-helper");
  ASSERT_TRUE(Send(conversation, "Start the review helper.").Code().empty());
  ASSERT_TRUE(model.WaitForRequests(2));
  ASSERT_TRUE(Call(*external, A::RequestPayload::ConversationCancelRequest,
                   [&](Builder& b) {
                     return A::CreateConversationCancelRequest(
                                b, b.CreateString(conversation))
                         .Union();
                   })
                  .Code()
                  .empty());
  const auto stopped = WaitUntilIdle(conversation);
  EXPECT_FALSE(JsonBool(stopped, "/resumable"));
  EXPECT_TRUE(JsonBool(stopped, "/continuable"));
  EXPECT_EQ(JsonString(stopped, "/conversation/helpers/0/state"),
            "interrupted");
  EXPECT_NE(stopped.find("Prepare a bounded review."), std::string::npos);
  EXPECT_EQ(stopped.find("This cancelled helper answer must not be saved."),
            std::string::npos);
  model.Release();
  external.reset();
  human.reset();
  service.Stop();
  StartHost();
  EXPECT_EQ(
      JsonString(Get(conversation).View(), "/conversation/helpers/0/state"),
      "interrupted");
  ASSERT_TRUE(Call(*external, A::RequestPayload::ConversationActionRequest,
                   [&](Builder& b) {
                     return A::CreateConversationActionRequest(
                                b, b.CreateString(conversation),
                                A::Action::Resume)
                         .Union();
                   })
                  .Code()
                  .empty());
  ASSERT_TRUE(model.WaitForRequests(3));
  EXPECT_NE(WaitUntilIdle(conversation)
                .find("The parent acknowledged the interrupted helper."),
            std::string::npos);
  EXPECT_EQ(model.Requests().size(), 3u);
  EXPECT_EQ(
      JsonString(Get(conversation).View(), "/conversation/helpers/0/state"),
      "interrupted");
}

TEST_F(AgentFixture, HelpersRequireScopeUnlessThePersonAllowsUnscopedWork) {
  ModelServer model;
  Configure(model);
  const auto conversation = Conversation("helper-policy");
  const std::string task =
      R"({"tasks":[{"name":"review","instruction":"Review only this instruction."}]})";
  EXPECT_EQ(Tool(conversation, "delegate", task, "unscoped-denied").Code(),
            "invalid_request");
  EXPECT_TRUE(model.Requests().empty());
  ASSERT_TRUE(
      SettingsPatch(*host, "desktop", {{"agent.allowUnscopedHelpers", "true"}})
          .Code()
          .empty());
  model.Queue(AnthropicText("The unscoped task is complete."));
  EXPECT_TRUE(
      Tool(conversation, "delegate", task, "unscoped-allowed").Code().empty());
  EXPECT_EQ(model.Requests().size(), 1u);
  EXPECT_EQ(
      Tool(
          conversation, "delegate",
          R"({"tasks":[{"name":"partial","instruction":"Review.","read_cards":[]}]})",
          "partial-scope")
          .Code(),
      "invalid_request");
  EXPECT_EQ(model.Requests().size(), 1u);
}

TEST_F(AgentFixture, RevokingUnscopedPermissionAlsoBlocksScopedChildren) {
  ModelServer model;
  Configure(model);
  const auto conversation = Conversation("revoke-unscoped-parent");
  ASSERT_TRUE(
      SettingsPatch(*host, "desktop", {{"agent.allowUnscopedHelpers", "true"}})
          .Code()
          .empty());
  model.Queue(
      AnthropicTool(
          "scoped-child", "delegate",
          R"({"tasks":[{"name":"child","instruction":"Read any Card.","read_cards":["arbitrary"],"write_cards":[],"run_cards":[]}]})"),
      true);
  model.Queue(AnthropicText("Parent saw the permission refusal."));
  auto pending = std::async(std::launch::async, [&] {
    return Tool(
        conversation, "delegate",
        R"({"tasks":[{"name":"parent","instruction":"Delegate.","depth":1,"count":1}]})",
        "unscoped-parent");
  });
  const bool started = model.WaitForRequests(1);
  if (!started)
    model.Release();
  ASSERT_TRUE(started);
  const auto revoked = SettingsPatch(*host, "desktop",
                                     {{"agent.allowUnscopedHelpers", "false"}});
  model.Release();
  ASSERT_TRUE(revoked.Code().empty());
  ASSERT_TRUE(pending.get().Code().empty());
  const auto requests = model.Requests();
  ASSERT_EQ(requests.size(), 2u);
  EXPECT_NE(requests.back().find("Unscoped helpers are disabled"),
            std::string::npos);
  EXPECT_EQ(ListSize(Get(conversation)
                         .Value()
                         ->value_as_ConversationResult()
                         ->conversation()
                         ->helpers()),
            1u);
}

TEST_F(AgentFixture, MainAgentStartsAllTasksAcrossDelegateCalls) {
  // One watchdog for both cases, including held requests and their cleanup.
  const auto deadline = std::chrono::steady_clock::now() + 20s;
  for (const auto limit : {4}) {
    SCOPED_TRACE(limit);
    ModelServer model;
    Configure(model);
    const auto conversation =
        Conversation("helper-parallel-" + std::to_string(limit));
    model.Queue(AnthropicText("First helper completed."), true);
    for (int i = 0; i < 3; ++i)
      model.Queue(AnthropicText("Another helper completed."));
    const std::string tasks =
        R"({"tasks":[{"name":"one","instruction":"First task.","read_cards":[],"write_cards":[],"run_cards":[]},{"name":"two","instruction":"Second task.","read_cards":[],"write_cards":[],"run_cards":[]}]})";
    bool completed = false;
    const auto cleanup = [&](void*) {
      model.Release();
      // A timed-out client does not cancel an external delegate call. Kill
      // this fixture's service before destroying the provider, so outstanding
      // helpers cannot retry against a dead provider during teardown.
      if (!completed) service.Stop(true);
    };
    std::unique_ptr<void, decltype(cleanup)> guard(this, cleanup);
    auto delegate = [&](const std::string& operation) {
      return external->Call(
          [&](uint64_t id) {
            Builder b;
            const auto value = A::CreateToolCallRequest(
                b, b.CreateString(conversation), b.CreateString(project_id),
                b.CreateString("delegate"), b.CreateString(tasks),
                b.CreateString(operation));
            A::FinishMessageBuffer(
                b, A::CreateMessage(
                       b, id, A::Payload::Request,
                       A::CreateRequest(b, A::RequestPayload::ToolCallRequest,
                                        value.Union())
                           .Union()));
            return client::Bytes(b.GetBufferPointer(),
                                 b.GetBufferPointer() + b.GetSize());
          },
          std::chrono::duration_cast<std::chrono::milliseconds>(
              deadline - std::chrono::steady_clock::now()));
    };
    // Native pending calls have non-blocking destructors. std::async futures
    // join on destruction, including when an assertion abandons the test.
    auto first = delegate("batch-one");
    auto second = delegate("batch-two");
    ASSERT_TRUE(model.WaitForRequests(
        1, std::chrono::duration_cast<std::chrono::milliseconds>(
               deadline - std::chrono::steady_clock::now())))
        << "No helper reached the held provider";
    // The provider holds its first reply. Count started, persisted helpers,
    // rather than HTTP requests: this fixture serves one HTTP peer at a time.
    auto started = [&] {
      const auto reply = Get(conversation);
      return ListSize(reply.Value()
                          ->value_as_ConversationResult()
                          ->conversation()
                          ->helpers());
    };
    auto wait_started = [&](size_t count) {
      size_t seen = 0;
      do {
        try {
          seen = started();
        } catch (const std::exception& error) {
          throw std::runtime_error("Waiting for " + std::to_string(count) +
                                   " persisted helpers: " + error.what());
        }
        if (seen >= count) return seen;
        std::this_thread::sleep_for(10ms);
      } while (std::chrono::steady_clock::now() < deadline);
      return seen;
    };
    ASSERT_EQ(wait_started(static_cast<size_t>(limit)),
              static_cast<size_t>(limit))
        << "Waiting for all helpers to start";
    model.Release();
    const auto finish = [&](std::future<client::Result>& pending,
                            const char* batch) -> Reply {
      if (pending.wait_until(deadline) != std::future_status::ready)
        throw std::runtime_error(
            std::string(batch) +
            " did not finish after releasing the provider\n" +
            Read(directory.path / "host.log"));
      auto result = pending.get();
      if (result.error)
        throw std::runtime_error(std::string(batch) + ": " +
                                 result.error.message + "\n" +
                                 Read(directory.path / "host.log"));
      return {std::move(result.bytes)};
    };
    ASSERT_TRUE(finish(first, "batch-one").Code().empty());
    ASSERT_TRUE(finish(second, "batch-two").Code().empty());
    completed = true;
    EXPECT_EQ(model.Requests().size(), 4u);
    EXPECT_EQ(
        JsonUInt(Get(conversation).View(), "/conversation/usage/inputTokens"),
        48u);
  }
}

TEST_F(AgentFixture,
       SubAgentLifetimeCountIsEnforcedAndNestedTasksStayUnderParent) {
  ModelServer model;
  Configure(model);
  const auto conversation = Conversation("nested-capabilities");
  const std::string child =
      R"({"tasks":[{"name":"child","instruction":"Return a result.","read_cards":[],"write_cards":[],"run_cards":[]}]})";
  model.Queue(AnthropicTool("first-child", "delegate", child));
  model.Queue(AnthropicText("Child finished."));
  auto refused = child;
  refused.replace(refused.find("child"), 5, "refused");
  model.Queue(AnthropicTool("second-child", "delegate", refused));
  model.Queue(AnthropicText("Parent finished after the count refusal."));
  const auto reply = Tool(
      conversation, "delegate",
      R"({"tasks":[{"name":"parent","instruction":"Delegate twice.","read_cards":[],"write_cards":[],"run_cards":[],"depth":1,"count":1}]})",
      "start-parent");
  ASSERT_TRUE(reply.Code().empty()) << reply.View();
  const auto requests = model.Requests();
  ASSERT_EQ(requests.size(), 4u);
  EXPECT_NE(requests.back().find("lifetime child count exhausted"),
            std::string::npos);
  const auto saved = Get(conversation);
  const auto* value =
      saved.Value()->value_as_ConversationResult()->conversation();
  ASSERT_EQ(ListSize(value->helpers()), 2u);
  std::function<size_t(
      const flatbuffers::Vector<flatbuffers::Offset<A::ChatRow>>*, size_t)>
      count;
  count = [&](const auto* rows, size_t level) -> size_t {
    size_t found = 0;
    if (rows)
      for (const auto* row : *rows) {
        if (row->kind()->str() == "helper" && row->name()->str() == "child") {
          EXPECT_GT(level, 0u);
          ++found;
        }
        found += count(row->children(), level + 1);
      }
    return found;
  };
  EXPECT_EQ(count(value->rows(), 0), 1u);
  // A retry of the same parent task returns its receipt without spawning again.
  ASSERT_TRUE(
      Tool(
          conversation, "delegate",
          R"({"tasks":[{"name":"parent","instruction":"Delegate twice.","read_cards":[],"write_cards":[],"run_cards":[],"depth":1,"count":1}]})",
          "start-parent")
          .Code()
          .empty());
  EXPECT_EQ(model.Requests().size(), 4u);
}

TEST_F(AgentFixture, DefaultSubAgentCannotDelegateEvenIfTheModelCallsIt) {
  ModelServer model;
  Configure(model);
  const auto conversation = Conversation("leaf-capability");
  model.Queue(AnthropicTool(
      "forbidden-child", "delegate",
      R"({"tasks":[{"name":"forbidden","instruction":"Return.","read_cards":[],"write_cards":[],"run_cards":[]}]})"));
  model.Queue(AnthropicText("The leaf returned."));
  ASSERT_TRUE(
      Tool(
          conversation, "delegate",
          R"({"tasks":[{"name":"leaf","instruction":"Try delegating.","read_cards":[],"write_cards":[],"run_cards":[]}]})",
          "start-leaf")
          .Code()
          .empty());
  EXPECT_EQ(model.Requests().size(), 2u);
  EXPECT_EQ(ListSize(Get(conversation)
                         .Value()
                         ->value_as_ConversationResult()
                         ->conversation()
                         ->helpers()),
            1u);
}

TEST_F(AgentFixture, ShortenedHelperResultsPointToFullTraceAndSumUsageOnce) {
  ModelServer model;
  model.Queue(AnthropicTool(
      "long-helper", "delegate",
      R"({"tasks":[{"name":"long","instruction":"Produce a long result.","read_cards":[],"write_cards":[],"run_cards":[]}]})"));
  const auto long_text = std::string(70000, 'x') + "FULL_HELPER_TRACE_END";
  auto helper_stream = AnthropicText(long_text);
  const auto at = helper_stream.find("\"input_tokens\":12");
  ASSERT_NE(at, std::string::npos);
  helper_stream.replace(at, std::string("\"input_tokens\":12").size(),
                        "\"input_tokens\":12,\"cache_read_input_tokens\":7,"
                        "\"cache_creation_input_tokens\":3");
  model.Queue(helper_stream);
  model.Queue(AnthropicText("The helper result is retained."));
  Configure(model);
  const auto conversation = Conversation("long-helper-usage");
  ASSERT_TRUE(Send(conversation, "Delegate the review.").Code().empty());
  const auto state = WaitUntilIdle(conversation);
  const auto requests = model.Requests();
  ASSERT_EQ(requests.size(), 3u);
  EXPECT_EQ(JsonString(requests[0], "/messages/0/content/0/text"),
            "Delegate the review.");
  // Tool results carry their payload in content, not a text block's text.
  EXPECT_EQ(JsonString(requests[2], "/messages/2/content/0/type"),
            "tool_result");
  EXPECT_EQ(JsonString(requests[2], "/messages/2/content/0/tool_use_id"),
            "long-helper");
  EXPECT_FALSE(JsonBool(requests[2], "/messages/2/content/0/is_error"));
  const auto result = JsonString(requests[2], "/messages/2/content/0/content");
  EXPECT_TRUE(JsonBool(result, "/0/shortened"));
  EXPECT_EQ(JsonString(result, "/0/conversation_id"), conversation);
  EXPECT_EQ(JsonString(result, "/0/call_id"), "long-helper");
  EXPECT_NE(JsonString(result, "/0/notice").find("full"), std::string::npos);
  EXPECT_LE(JsonString(result, "/0/report").size(), 8192u);
  EXPECT_TRUE(
      JsonString(result, "/0/report").starts_with(std::string(1000, 'x')));
  EXPECT_EQ(result.find("FULL_HELPER_TRACE_END"), std::string::npos);
  EXPECT_NE(state.find("FULL_HELPER_TRACE_END"), std::string::npos);
  EXPECT_EQ(
      JsonString(state,
                 "/conversation/helpers/0/result/messages/1/content/0/text"),
      long_text);
  // Inputs accumulate: parent (12 + 12) + helper (12 + 7 + 3).
  // Outputs accumulate across every reply: delegate 8 + helper 4 + parent 4.
  EXPECT_EQ(JsonUInt(state, "/conversation/usage/inputTokens"), 46u);
  EXPECT_EQ(JsonUInt(state, "/conversation/usage/outputTokens"), 16u);
  EXPECT_EQ(JsonUInt(state, "/conversation/usage/cacheReadTokens"), 7u);
  EXPECT_EQ(JsonUInt(state, "/conversation/usage/cacheWriteTokens"), 3u);
  // Replaying the completed delegation returns its receipt without another
  // model call or charging the helper a second time.
  ASSERT_TRUE(
      Tool(
          conversation, "delegate",
          R"({"tasks":[{"name":"long","instruction":"Produce a long result.","read_cards":[],"write_cards":[],"run_cards":[]}]})",
          "long-helper")
          .Code()
          .empty());
  EXPECT_EQ(model.Requests().size(), 3u);
  const auto replayed = Get(conversation).View();
  EXPECT_EQ(JsonUInt(replayed, "/conversation/usage/inputTokens"), 46u);
  EXPECT_EQ(JsonUInt(replayed, "/conversation/usage/outputTokens"), 16u);
  EXPECT_EQ(JsonUInt(replayed, "/conversation/usage/cacheReadTokens"), 7u);
  EXPECT_EQ(JsonUInt(replayed, "/conversation/usage/cacheWriteTokens"), 3u);
  model.Queue(AnthropicText("A separate turn."));
  ASSERT_TRUE(Send(conversation, "Continue.", "turn-2").Code().empty());
  const auto next = WaitUntilIdle(conversation);
  EXPECT_EQ(JsonUInt(next, "/conversation/usage/inputTokens"), 58u);
  EXPECT_EQ(JsonUInt(next, "/conversation/usage/outputTokens"), 20u);
  EXPECT_EQ(JsonUInt(next, "/conversation/usage/cacheReadTokens"), 7u);
  EXPECT_EQ(JsonUInt(next, "/conversation/usage/cacheWriteTokens"), 3u);
}

}  // namespace
}  // namespace tsugumi::test
