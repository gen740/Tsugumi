// Scenarios exercise only the public Agent RPC through the shipped client.
#include "fixture.hh"
#include "tsugumi/protocol/tsugumi_generated.h"

namespace tsugumi::test {
namespace {
namespace R = Tsugumi::RPC;

TEST_F(AgentFixture, UnixAndWebSocketExposeTheSameAgentProtocol) {
  auto websocket = Connect(false, true);
  const auto unix_catalog =
      Call(*external, A::RequestPayload::ToolCatalogRequest, [](Builder& b) {
        return A::CreateToolCatalogRequest(b).Union();
      }).Json();
  const auto websocket_catalog =
      Call(*websocket, A::RequestPayload::ToolCatalogRequest, [](Builder& b) {
        return A::CreateToolCatalogRequest(b).Union();
      }).Json();
  EXPECT_EQ(unix_catalog, websocket_catalog);
  EXPECT_NE(unix_catalog.find("project_inspect"), std::string::npos);
  EXPECT_NE(unix_catalog.find("delegate"), std::string::npos);
  EXPECT_NE(unix_catalog.find("guide_show"), std::string::npos);
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
  EXPECT_TRUE(agent.Alive());
  EXPECT_TRUE(
      Call(*external, A::RequestPayload::DemoListRequest,
           [](Builder& b) { return A::CreateDemoListRequest(b).Union(); })
          .Code()
          .empty());
}

TEST_F(AgentFixture, ExternalAgentsCannotConfigureProvidersOrRegisterProjects) {
  EXPECT_EQ(
      Call(*external, A::RequestPayload::ConfigureRequest,
           [](Builder& b) {
             return A::CreateConfigureRequest(
                        b, b.CreateString(
                               R"({"provider":"fixture","model":"injected"})"))
                 .Union();
           })
          .Code(),
      "permission_denied");
  EXPECT_EQ(Call(*external, A::RequestPayload::RegisterProjectRequest,
                 [](Builder& b) {
                   return A::CreateRegisterProjectRequest(
                              b, b.CreateString("untrusted"),
                              b.CreateString("unix:/tmp/anything"))
                       .Union();
                 })
                .Code(),
            "permission_denied");
}

TEST_F(AgentFixture, InvalidRequestIdsAndActionsLeaveTheConversationUnchanged) {
  ModelServer model;
  Configure(model);
  const auto conversation = Conversation("invalid-request-values");
  const auto initial = Get(conversation).Json();
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
  EXPECT_EQ(Get(conversation).Json(), initial);
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
           [](Builder& b) {
             return A::CreateConversationListRequest(b).Union();
           })
          .Json();
  EXPECT_EQ(visible.find(conversation), std::string::npos);
  EXPECT_TRUE(Get(conversation, true).Code().empty());
}

TEST_F(AgentFixture,
       ExternalAgentsCannotProvisionReadOrRevokeModelCredentials) {
  EXPECT_EQ(Call(*external, A::RequestPayload::CredentialImportRequest,
                 [](Builder& b) {
                   return A::CreateCredentialImportRequest(
                              b, b.CreateString("{}"), true)
                       .Union();
                 })
                .Code(),
            "permission_denied");
  EXPECT_EQ(Call(*external, A::RequestPayload::CredentialTokenRequest,
                 [](Builder& b) {
                   return A::CreateCredentialTokenRequest(
                              b, b.CreateString("fixture-client"))
                       .Union();
                 })
                .Code(),
            "permission_denied");
  EXPECT_EQ(Call(*external, A::RequestPayload::CredentialForgetRequest,
                 [](Builder& b) {
                   return A::CreateCredentialForgetRequest(
                              b, b.CreateString("fixture-client"))
                       .Union();
                 })
                .Code(),
            "permission_denied");
}

TEST_F(AgentFixture, CorePersistsRotatedCredentialsAndRevokesTheCurrentToken) {
  ModelServer authorization;
  authorization.QueueHTTP(
      R"({"access_token":"access-new","refresh_token":"refresh-new","token_type":"Bearer","scope":"chatgpt.tokens.use.direct","expires_in":3600})");
  authorization.QueueHTTP("", 204);
  ASSERT_TRUE(ImportCredential(authorization).Code().empty());
  EXPECT_EQ(JsonString(Token().Json(), "/access_token"), "access-new");
  ASSERT_TRUE(authorization.WaitForRequests(1));
  EXPECT_EQ(JsonString(Token("access-old").Json(), "/access_token"),
            "access-new");
  // A second UI opening with an old cached registration must not undo rotation.
  ASSERT_TRUE(ImportCredential(authorization).Code().empty());
  external.reset();
  human.reset();
  agent.Stop();
  StartAgent();
  EXPECT_EQ(JsonString(Token().Json(), "/access_token"), "access-new");
  EXPECT_EQ(authorization.Requests().size(), 1u);
  const auto forgotten =
      Call(*human, A::RequestPayload::CredentialForgetRequest, [](Builder& b) {
        return A::CreateCredentialForgetRequest(
                   b, b.CreateString("fixture-client"))
            .Union();
      });
  ASSERT_TRUE(forgotten.Code().empty());
  EXPECT_TRUE(JsonBool(forgotten.Json(), "/revoked"));
  ASSERT_TRUE(authorization.WaitForRequests(2));
  const auto requests = authorization.Requests();
  EXPECT_NE(requests[0].find("refresh_token=refresh-old"), std::string::npos);
  EXPECT_NE(requests[0].find("resource=https%3A%2F%2Ffixture.invalid%2Fapi"),
            std::string::npos);
  EXPECT_NE(requests[1].find("token=refresh-new"), std::string::npos);
  EXPECT_EQ(requests[1].find("refresh-old"), std::string::npos);
  EXPECT_NE(
      authorization.Headers()[0].find("application/x-www-form-urlencoded"),
      std::string::npos);
  EXPECT_EQ(Token().Code(), "not_configured");
}

TEST_F(AgentFixture, CredentialRefreshErrorsDoNotExposeTheSubmittedToken) {
  ModelServer authorization;
  authorization.QueueHTTP(
      R"({"error":"invalid_grant","description":"rejected refresh-old"})", 400);
  ASSERT_TRUE(ImportCredential(authorization).Code().empty());
  const auto rejected = Token();
  EXPECT_FALSE(rejected.Code().empty());
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(rejected.bytes.data()),
                        rejected.bytes.size())
                .find("refresh-old"),
            std::string::npos);
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
  const auto config =
      "{\"provider\":\"chatgpt\",\"token\":\"fixture-access\","
      "\"baseURL\":" +
      Quote(model.URL()) + ",\"model\":\"fixture-model\"}";
  ASSERT_TRUE(
      Call(
          *human, A::RequestPayload::ConfigureRequest,
          [&](Builder& b) {
            return A::CreateConfigureRequest(b, b.CreateString(config)).Union();
          })
          .Code()
          .empty());
  for (int turn = 0; turn < 2; ++turn) {
    const auto conversation =
        Conversation("plan-limit-" + std::to_string(turn));
    ASSERT_TRUE(Send(conversation, "Respond on the plan.").Code().empty());
    ASSERT_TRUE(model.WaitForRequests(turn + 1));
    WaitUntilIdle(conversation);
    const auto events = Events(conversation).Json();
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
  const auto good =
      Call(*external, A::RequestPayload::DemoReadRequest, [](Builder& b) {
        return A::CreateDemoReadRequest(b, b.CreateString("ja/example.json"))
            .Union();
      });
  EXPECT_NE(good.Json().find("BUNDLED_DEMO"), std::string::npos);
  for (const auto* path : {"../private.json", "escape.json", "/private.json"}) {
    SCOPED_TRACE(path);
    const auto reply =
        Call(*external, A::RequestPayload::DemoReadRequest, [&](Builder& b) {
          return A::CreateDemoReadRequest(b, b.CreateString(path)).Union();
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
  EXPECT_NE(Events(conversation).Json().find("finished"), std::string::npos);
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
  EXPECT_NE(Events(conversation).Json().find("cancelled"), std::string::npos);
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
  const auto events = Events(conversation).Json();
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
  EXPECT_EQ(Events(conversation).Json().find("\"kind\":\"toolStarted\""),
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
  EXPECT_NE(Events(conversation).Json().find("empty reply"), std::string::npos);
  ASSERT_TRUE(
      Send(conversation, "Try another request.", "turn-2").Code().empty());
  ASSERT_TRUE(model.WaitForRequests(2));
  EXPECT_NE(WaitUntilIdle(conversation)
                .find("A later request can still be answered."),
            std::string::npos);
  EXPECT_EQ(model.Requests()[1].find("\"content\":[]"), std::string::npos);
}

TEST_F(AgentFixture,
       AutomaticCompactionKeepsTheArchiveAndContinuesTheConversation) {
  ModelServer model;
  const auto earlier = "FIRST_LONG_ANSWER " + std::string(1500, 'a');
  model.Queue(AnthropicText(earlier));
  model.Queue(AnthropicText("SECOND_LONG_ANSWER " + std::string(1500, 'b')));
  model.Queue(AnthropicText("THIRD_LONG_ANSWER " + std::string(1500, 'c')));
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
  Configure(model, "fixture", 4000);
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
  EXPECT_NE(requests[3].find("Summarize the quoted earlier conversation"),
            std::string::npos);
  EXPECT_EQ(requests[4].find(earlier), std::string::npos);
  EXPECT_NE(requests[4].find("Earlier decisions were retained"),
            std::string::npos);
  EXPECT_NE(Events(conversation).Json().find("compacted"), std::string::npos);
  const auto found =
      Call(*external, A::RequestPayload::ConversationSearchRequest,
           [](Builder& b) {
             return A::CreateConversationSearchRequest(
                        b, 0, b.CreateString("first_long_answer"))
                 .Union();
           })
          .Json();
  EXPECT_EQ(JsonString(found, "/0/session"), conversation);
  const auto snippet = JsonString(found, "/0/snippet");
  EXPECT_NE(snippet.find("FIRST_LONG_ANSWER"), std::string::npos);
  EXPECT_LT(snippet.size(), earlier.size());
  EXPECT_EQ(model.Requests().size(), 5u);
}

TEST_F(AgentFixture, OversizedDelegationsAreRefusedBeforeStartingAnyHelper) {
  ModelServer model;
  std::string tasks = "{\"tasks\":[";
  for (int i = 0; i < 9; ++i) {
    if (i) tasks += ',';
    tasks += "{\"name\":\"helper-" + std::to_string(i) +
             "\",\"instruction\":\"Do a bounded task.\",\"cards\":[]}";
  }
  tasks += "]}";
  model.Queue(AnthropicTool("oversized-delegation", "delegate", tasks));
  model.Queue(AnthropicText("The oversized delegation was refused."));
  Configure(model);
  const auto conversation = Conversation("bounded-delegation");
  ASSERT_TRUE(Send(conversation, "Start the helpers.").Code().empty());
  ASSERT_TRUE(model.WaitForRequests(2));
  EXPECT_NE(
      WaitUntilIdle(conversation).find("The oversized delegation was refused."),
      std::string::npos);
  EXPECT_EQ(model.Requests().size(), 2u);
  EXPECT_TRUE(JsonBool(model.Requests()[1], "/messages/2/content/0/is_error"));
}

TEST_F(AgentFixture,
       CancellingAHelperPreservesItsTraceWithoutReplayingItOnResume) {
  ModelServer model;
  model.Queue(AnthropicTool(
      "cancelled-delegation", "delegate",
      R"({"tasks":[{"name":"review","instruction":"Prepare a bounded review."}]})"));
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
  agent.Stop();
  StartAgent();
  EXPECT_EQ(
      JsonString(Get(conversation).Json(), "/conversation/helpers/0/state"),
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
      JsonString(Get(conversation).Json(), "/conversation/helpers/0/state"),
      "interrupted");
}

}  // namespace
}  // namespace tsugumi::test
