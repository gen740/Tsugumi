// Behavior scenarios use the same native client as the CLI. Only public
// FlatBuffers requests, replies and notifications are inspected.
#include <flatbuffers/flatbuffers.h>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "agent_fixture_server.hh"
#include "tsugumi/client/client.hh"
#include "tsugumi/protocol/agent_generated.h"
#include "tsugumi/protocol/tsugumi_generated.h"

namespace tsugumi::test {
namespace {

namespace A = Tsugumi::Agent;
namespace R = Tsugumi::RPC;
using namespace std::chrono_literals;
using Builder = flatbuffers::FlatBufferBuilder;
using Build = std::function<flatbuffers::Offset<void>(Builder&)>;

struct Reply {
  client::Bytes bytes;
  auto Value() const -> const A::Reply* {
    const auto* message = A::GetMessage(bytes.data());
    const auto* reply = message->payload_as_Reply();
    if (!reply) throw std::runtime_error("Agent returned no reply payload");
    return reply;
  }
  auto Code() const -> std::string {
    const auto* failure = Value()->value_as_Failure();
    return failure ? failure->code()->str() : "";
  }
  auto RequestId() const -> std::string {
    const auto* failure = Value()->value_as_Failure();
    return failure && failure->request_id() ? failure->request_id()->str() : "";
  }
  auto Json() const -> std::string {
    const auto* result = Value()->value_as_Result();
    if (!result) {
      const auto* failure = Value()->value_as_Failure();
      throw std::runtime_error(failure ? failure->code()->str() + ": " +
                                             failure->message()->str()
                                       : "Expected a JSON result");
    }
    return result->json()->str();
  }
};

auto Call(client::Connection& connection, A::RequestPayload type,
          const Build& build) -> Reply {
  auto pending = connection.Call(
      [&](uint64_t id) {
        Builder builder;
        const auto value = build(builder);
        const auto request = A::CreateRequest(builder, type, value);
        A::FinishMessageBuffer(
            builder, A::CreateMessage(builder, id, A::Payload::Request,
                                      request.Union()));
        return client::Bytes(builder.GetBufferPointer(),
                             builder.GetBufferPointer() + builder.GetSize());
      },
      5s);
  if (pending.wait_for(6s) != std::future_status::ready)
    throw std::runtime_error("Agent RPC did not finish before its deadline");
  auto result = pending.get();
  if (result.error)
    throw std::runtime_error("Agent transport: " + result.error.message);
  return {std::move(result.bytes)};
}

class AgentRPC : public ::testing::Test {
 protected:
  auto SetUp() -> void override {
    Write(directory.path / "demos/ja/example.json",
          R"({"example":"BUNDLED_DEMO"})");
    Write(directory.path / "private.json",
          R"({"secret":"PRIVATE_OUTSIDE_DEMO"})");
    std::filesystem::create_symlink(directory.path / "private.json",
                                    directory.path / "demos/escape.json");
    StartAgent();
  }

  auto TearDown() -> void override {
    external.reset();
    human.reset();
    agent.Stop();
    project.Stop();
  }

  auto StartAgent() -> void {
    std::filesystem::remove(directory.path / "state/agent.ready");
    agent.Start(
        {TSUGUMI_AGENT_BINARY, "--state-dir",
         (directory.path / "state").string(), "--unix",
         (directory.path / "agent.sock").string(), "--demos-dir",
         (directory.path / "demos").string(), "--listen", "127.0.0.1:0"},
        directory.path / "agent.log");
    ready = Readiness(agent, directory.path / "state/agent.ready",
                      directory.path / "agent.log");
    human = Connect(true);
    external = Connect(false);
  }

  auto Connect(bool user, bool websocket = false)
      -> std::unique_ptr<client::Connection> {
    client::Options options;
    options.protocol = client::Protocol::Agent;
    options.endpoint = websocket ? JsonString(ready, "/endpoint")
                                 : "unix:" + JsonString(ready, "/unix_socket");
    options.connect_timeout = 3s;
    auto connection = std::make_unique<client::Connection>(options);
    auto opened = connection->Start();
    if (opened.wait_for(4s) != std::future_status::ready)
      throw std::runtime_error("Agent connection did not open");
    if (auto error = opened.get()) throw std::runtime_error(error.message);
    auto hello =
        Call(*connection, A::RequestPayload::HelloRequest, [&](Builder& b) {
          return A::CreateHelloRequest(
                     b,
                     b.CreateString(JsonString(
                         ready, user ? "/user_token" : "/external_token")),
                     b.CreateString("public-rpc-scenario"))
              .Union();
        });
    if (!hello.Value()->value_as_HelloResult())
      throw std::runtime_error("Agent rejected a valid client credential");
    return connection;
  }

  auto StartProject() -> void {
    Write(
        directory.path / "project/tsugumi.json",
        R"({"name":"PRIVATE_PROJECT_SENTINEL","pkgsPath":"tsugumi:default","databaseConnections":[],"canvas":{"cards":[],"connections":[]}})");
    Write(directory.path / "project/layout.json", "{}");
    project.Start(
        {TSUGUMI_PROJECT_BINARY, "--project",
         (directory.path / "project").string(), "--unix",
         (directory.path / "project.sock").string(), "--listen", "127.0.0.1:0",
         "--control-dir", (directory.path / "project-control").string(),
         "--worker", TSUGUMI_WORKER_BINARY, "--sdk-dir", TSUGUMI_TEST_SDK},
        directory.path / "project.log");
    project_ready =
        Readiness(project, directory.path / "project-control/server.ready",
                  directory.path / "project.log");
    project_id = JsonString(project_ready, "/project_id");
    auto reply = Call(
        *human, A::RequestPayload::RegisterProjectRequest, [&](Builder& b) {
          return A::CreateRegisterProjectRequest(
                     b, b.CreateString(project_id),
                     b.CreateString("unix:" +
                                    JsonString(project_ready, "/unix_socket")),
                     b.CreateString(JsonString(project_ready, "/token")),
                     b.CreateString("Private project"))
              .Union();
        });
    if (!reply.Code().empty())
      throw std::runtime_error("cannot register test project: " + reply.Code());
  }

  auto StartImageProject(bool complete) -> void {
    static constexpr char pixel[] =
        "\x89\x50\x4e\x47\x0d\x0a\x1a\x0a\x00\x00\x00\x0d\x49\x48\x44\x52"
        "\x00\x00\x00\x01\x00\x00\x00\x01\x08\x06\x00\x00\x00\x1f\x15\xc4\x89"
        "\x00\x00\x00\x0d\x49\x44\x41\x54\x78\x9c\x63\xf8\xcf\xc0\xf0\x1f"
        "\x00\x05\x00\x01\xff\x89\x99\x3d\x1d\x00\x00\x00\x00\x49\x45\x4e\x44"
        "\xae\x42\x60\x82";
    Write(directory.path / "project/definitions/picture/_card.json",
          R"({"template":"draft"})");
    Write(directory.path / "project/definitions/picture/src/pixel.png",
          std::string_view(pixel, sizeof(pixel) - 1 - (complete ? 0 : 8)));
    StartProject();
  }

  auto Conversation(std::string id, bool user = false) -> std::string {
    auto reply =
        Call(user ? *human : *external,
             A::RequestPayload::ConversationCreateRequest, [&](Builder& b) {
               return A::CreateConversationCreateRequest(
                          b, b.CreateString(id),
                          project_id.empty() ? 0 : b.CreateString(project_id))
                   .Union();
             });
    return JsonString(reply.Json(), "/conversation/id");
  }

  auto Inspect(const std::string& conversation, std::string operation = {})
      -> Reply {
    if (operation.empty())
      operation = "inspect-" + std::to_string(++operation_id);
    return Call(*external, A::RequestPayload::ToolCallRequest, [&](Builder& b) {
      return A::CreateToolCallRequest(
                 b, b.CreateString(conversation), b.CreateString(project_id),
                 b.CreateString("project_inspect"), b.CreateString("{}"),
                 b.CreateString(operation))
          .Union();
    });
  }

  auto Tool(const std::string& conversation, const std::string& name,
            const std::string& arguments, std::string operation = {}) -> Reply {
    if (operation.empty()) operation = "tool-" + std::to_string(++operation_id);
    return Call(*external, A::RequestPayload::ToolCallRequest, [&](Builder& b) {
      return A::CreateToolCallRequest(
                 b, b.CreateString(conversation), b.CreateString(project_id),
                 b.CreateString(name), b.CreateString(arguments),
                 b.CreateString(operation))
          .Union();
    });
  }

  auto Grant(const std::string& conversation,
             A::GrantScope scope = A::GrantScope::Conversation,
             A::Access access = A::Access::Read) -> std::string {
    auto ask =
        Call(*external, A::RequestPayload::AccessRequest, [&](Builder& b) {
          return A::CreateAccessRequest(
                     b, b.CreateString(project_id),
                     b.CreateString(conversation), access, scope,
                     b.CreateString("Read this project for the requested work"))
              .Union();
        });
    const auto id = JsonString(ask.Json(), "/id");
    auto reply =
        Call(*human, A::RequestPayload::AccessResolveRequest, [&](Builder& b) {
          return A::CreateAccessResolveRequest(b, b.CreateString(id), true)
              .Union();
        });
    if (!reply.Code().empty())
      throw std::runtime_error("user could not grant access");
    return id;
  }

  auto Revoke(const std::string& grant) -> void {
    const auto reply =
        Call(*human, A::RequestPayload::AccessRevokeRequest, [&](Builder& b) {
          return A::CreateAccessRevokeRequest(b, b.CreateString(grant)).Union();
        });
    if (!reply.Code().empty())
      throw std::runtime_error("user could not revoke access");
  }

  auto Configure(const ModelServer& model, std::string provider = "fixture",
                 uint32_t context_limit = 600000) -> void {
    const auto config =
        "{\"provider\":" + Quote(provider) +
        ",\"auth\":\"none\",\"baseURL\":" + Quote(model.URL()) +
        ",\"model\":\"fixture-model\",\"maxTokens\":1024,\"contextLimit\":" +
        std::to_string(context_limit) + "}";
    auto reply =
        Call(*human, A::RequestPayload::ConfigureRequest, [&](Builder& b) {
          return A::CreateConfigureRequest(b, b.CreateString(config)).Union();
        });
    if (!reply.Code().empty())
      throw std::runtime_error("cannot configure external model fixture");
  }

  auto ImportCredential(const ModelServer& server, bool replace = false)
      -> Reply {
    const auto registration =
        "{\"client_id\":\"fixture-client\",\"refresh_token\":\"refresh-old\","
        "\"access_token\":\"access-old\",\"expires_at_ms\":0,"
        "\"resource\":\"https://fixture.invalid/api\",\"token_endpoint\":" +
        Quote(server.URL() + "/token") +
        ",\"revocation_endpoint\":" + Quote(server.URL() + "/revoke") + "}";
    return Call(*human, A::RequestPayload::CredentialImportRequest,
                [&](Builder& b) {
                  return A::CreateCredentialImportRequest(
                             b, b.CreateString(registration), replace)
                      .Union();
                });
  }

  auto Token(std::string_view stale = {}) -> Reply {
    return Call(
        *human, A::RequestPayload::CredentialTokenRequest, [&](Builder& b) {
          return A::CreateCredentialTokenRequest(
                     b, b.CreateString("fixture-client"), b.CreateString(stale))
              .Union();
        });
  }

  auto Send(const std::string& conversation, const std::string& text,
            std::string turn = "turn-1") -> Reply {
    return Call(*external, A::RequestPayload::ConversationSendRequest,
                [&](Builder& b) {
                  return A::CreateConversationSendRequest(
                             b, b.CreateString(conversation),
                             b.CreateString(turn), b.CreateString(text))
                      .Union();
                });
  }

  auto Get(const std::string& conversation, bool user = false) -> Reply {
    return Call(user ? *human : *external,
                A::RequestPayload::ConversationGetRequest, [&](Builder& b) {
                  return A::CreateConversationGetRequest(
                             b, b.CreateString(conversation))
                      .Union();
                });
  }

  auto Events(const std::string& conversation, bool user = false) -> Reply {
    return Call(user ? *human : *external, A::RequestPayload::EventsRequest,
                [&](Builder& b) {
                  return A::CreateEventsRequest(b, b.CreateString(conversation),
                                                0, 1000)
                      .Union();
                });
  }

  auto WaitUntilIdle(const std::string& conversation) -> std::string {
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    do {
      auto state = Get(conversation, true).Json();
      if (!JsonBool(state, "/busy")) return state;
      std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("Agent did not finish:\n" +
                             Events(conversation, true).Json() + "\n" +
                             Read(directory.path / "agent.log"));
  }

  TemporaryDirectory directory;
  ChildProcess agent;
  ChildProcess project;
  std::string ready;
  std::string project_ready;
  std::string project_id;
  std::unique_ptr<client::Connection> human;
  std::unique_ptr<client::Connection> external;
  uint64_t operation_id = 0;
};

TEST_F(AgentRPC, UnixAndWebSocketExposeTheSameAgentProtocol) {
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

TEST_F(AgentRPC, ProjectProtocolCannotBeUsedOnTheAgentSocket) {
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

TEST_F(AgentRPC, ExternalAgentsCannotConfigureProvidersOrRegisterProjects) {
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

TEST_F(AgentRPC, InvalidRequestIdsAndActionsLeaveTheConversationUnchanged) {
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

TEST_F(AgentRPC, ExternalAgentsCannotReadOrReplaceHumanConversations) {
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

TEST_F(AgentRPC, ExternalAgentsCannotProvisionReadOrRevokeModelCredentials) {
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

TEST_F(AgentRPC, CorePersistsRotatedCredentialsAndRevokesTheCurrentToken) {
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

TEST_F(AgentRPC, CredentialRefreshErrorsDoNotExposeTheSubmittedToken) {
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

TEST_F(AgentRPC, ChatGPTPlanLimitsStopWithoutRetryingAndUseThePlanWireFormat) {
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

TEST_F(AgentRPC, BundledDemosAreReadableButSymlinksAndTraversalAreNot) {
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

TEST_F(AgentRPC, ReadingAProjectRequiresAnExplicitUserGrant) {
  StartProject();
  const auto conversation = Conversation("permission-scenario");
  const auto denied = Inspect(conversation);
  EXPECT_EQ(denied.Code(), "grant_required");
  ASSERT_FALSE(denied.RequestId().empty());
  EXPECT_EQ(Call(*external, A::RequestPayload::AccessResolveRequest,
                 [&](Builder& b) {
                   return A::CreateAccessResolveRequest(
                              b, b.CreateString(denied.RequestId()), true)
                       .Union();
                 })
                .Code(),
            "permission_denied");
  Grant(conversation);
  const auto allowed = Inspect(conversation);
  EXPECT_TRUE(allowed.Code().empty());
  EXPECT_NE(allowed.Json().find("PRIVATE_PROJECT_SENTINEL"), std::string::npos);
}

TEST_F(AgentRPC, LookingUpAnOperationPreservesItsIdAndRequiresOnlyReadAccess) {
  StartProject();
  const auto conversation = Conversation("operation-lookup");
  Grant(conversation);
  const auto write_grant =
      Grant(conversation, A::GrantScope::Conversation, A::Access::Write);
  Revoke(write_grant);
  const auto unknown =
      Tool(conversation, "operation_get",
           R"({"op_id":"e38fd1b6-72fb-4df9-a91b-e6b8a72c6686"})");
  ASSERT_TRUE(unknown.Code().empty());
  EXPECT_FALSE(JsonBool(unknown.Json(), "/isError"));
  const auto lookup = JsonString(unknown.Json(), "/text");
  EXPECT_EQ(JsonString(lookup, "/reply"), "OperationResult");
  EXPECT_EQ(JsonString(lookup, "/value/state"), "unknown");
  // A malformed lookup ID must reach the project unchanged, rather than
  // being replaced by the Agent's own mutation ID.
  const auto invalid =
      Tool(conversation, "operation_get", R"({"op_id":"not-a-uuid"})");
  ASSERT_TRUE(invalid.Code().empty());
  EXPECT_TRUE(JsonBool(invalid.Json(), "/isError"));
  const auto failure = JsonString(invalid.Json(), "/text");
  EXPECT_EQ(JsonString(failure, "/reply"), "Failure");
  EXPECT_EQ(JsonString(failure, "/value/kind"), "invalid_request");
}

TEST_F(AgentRPC, ClearingProjectCacheUsesItsPublicRequestWithoutAddingOrigin) {
  StartProject();
  const auto conversation = Conversation("cache-clear");
  Grant(conversation);
  EXPECT_EQ(Tool(conversation, "project_cache_clear", "{}").Code(),
            "grant_required");
  Grant(conversation, A::GrantScope::Conversation, A::Access::Write);
  const auto cleared = Tool(conversation, "project_cache_clear", "{}");
  ASSERT_TRUE(cleared.Code().empty());
  EXPECT_FALSE(JsonBool(cleared.Json(), "/isError"));
  EXPECT_EQ(JsonString(JsonString(cleared.Json(), "/text"), "/reply"),
            "ProjectCacheClearResult");
}

TEST_F(AgentRPC, SavingAViewRequiresWritePermissionEvenWithoutAnOperationId) {
  StartProject();
  const auto conversation = Conversation("view-write-permission");
  Grant(conversation);
  const auto arguments =
      R"({"view_id":"9204fa0c-009f-49c9-af16-77641ee73ff7","base_view_rev":"0","view":{"scope":{"segments":[]},"cameras":[],"selected_cards":[]}})";
  EXPECT_EQ(Tool(conversation, "view_save", arguments).Code(),
            "grant_required");
  Grant(conversation, A::GrantScope::Conversation, A::Access::Write);
  const auto saved = Tool(conversation, "view_save", arguments);
  ASSERT_TRUE(saved.Code().empty());
  EXPECT_FALSE(JsonBool(saved.Json(), "/isError"));
  EXPECT_EQ(JsonString(JsonString(saved.Json(), "/text"), "/reply"),
            "ViewSaveResult");
}

TEST_F(AgentRPC, ConversationGrantsDoNotLeakIntoAnotherSessionAndCanBeRevoked) {
  StartProject();
  const auto first = Conversation("first");
  const auto second = Conversation("second");
  const auto grant = Grant(first);
  EXPECT_TRUE(Inspect(first).Code().empty());
  EXPECT_EQ(Inspect(second).Code(), "grant_required");
  Revoke(grant);
  EXPECT_EQ(Inspect(first).Code(), "grant_required");
}

TEST_F(AgentRPC,
       ServerScopedAccessRequiresApprovalAndCanBeRevokedAcrossSessions) {
  StartProject();
  const auto first = Conversation("first");
  const auto second = Conversation("second");
  const auto grant = Grant(first, A::GrantScope::Server);
  EXPECT_TRUE(Inspect(first).Code().empty());
  EXPECT_TRUE(Inspect(second).Code().empty());
  Revoke(grant);
  EXPECT_EQ(Inspect(first).Code(), "grant_required");
  EXPECT_EQ(Inspect(second).Code(), "grant_required");
}

TEST_F(AgentRPC, ConversationContinuesAfterTheClientDisconnects) {
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

TEST_F(AgentRPC, ConversationAndApprovedAccessSurviveAnAgentServerRestart) {
  ModelServer model;
  model.Queue(AnthropicText("This conversation is durable."));
  Configure(model);
  StartProject();
  const auto conversation = Conversation("durable");
  Grant(conversation);
  ASSERT_TRUE(Send(conversation, "Keep this conversation.").Code().empty());
  ASSERT_TRUE(model.WaitForRequests(1));
  WaitUntilIdle(conversation);
  external.reset();
  human.reset();
  agent.Stop();
  StartAgent();
  EXPECT_NE(Get(conversation).Json().find("This conversation is durable."),
            std::string::npos);
  EXPECT_NE(Inspect(conversation).Json().find("PRIVATE_PROJECT_SENTINEL"),
            std::string::npos);
}

TEST_F(AgentRPC, PublicCancellationStopsAnInFlightModelRequest) {
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

TEST_F(AgentRPC, OpenAIResponsesUsesTheSameCoreConversationService) {
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

TEST_F(AgentRPC, AnInterruptedModelStreamNeverExecutesItsToolCall) {
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

TEST_F(AgentRPC, OutputLimitDoesNotExecuteEvenAnOtherwiseCompleteToolCall) {
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

TEST_F(AgentRPC, InvalidToolArgumentsReturnAnErrorWithoutReadingTheDemo) {
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

TEST_F(AgentRPC, AnEmptyModelReplyDoesNotPreventTheNextConversationTurn) {
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

TEST_F(AgentRPC, AContextRefusalShrinksToolResultsBeforeAskingAgain) {
  ModelServer model;
  model.Queue(AnthropicTool("read-large-result", "project_file_read",
                            R"({"path":"skills/long.txt"})"));
  model.QueueHTTP(R"({"error":{"message":"prompt is too long"}})", 400);
  model.Queue(AnthropicText("The smaller context was accepted."));
  Configure(model, "fixture", 80000);
  Write(directory.path / "project/skills/long.txt", std::string(50000, 'x'));
  StartProject();
  const auto conversation = Conversation("context-refusal");
  Grant(conversation);
  ASSERT_TRUE(Send(conversation, "Read the long result.").Code().empty());
  ASSERT_TRUE(model.WaitForRequests(3));
  EXPECT_NE(
      WaitUntilIdle(conversation).find("The smaller context was accepted."),
      std::string::npos);
  const auto requests = model.Requests();
  EXPECT_LT(requests[2].size(), requests[1].size() - 20000);
  EXPECT_NE(requests[2].find("Cut to fit the model context"),
            std::string::npos);
  EXPECT_NE(Events(conversation).Json().find("compacted"), std::string::npos);
  // The full public receipt stays available even when the model sees a page.
  EXPECT_NE(Get(conversation).Json().find(std::string(50000, 'x')),
            std::string::npos);
}

TEST_F(AgentRPC,
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

TEST_F(AgentRPC, DelegatedWorkUsesTheParentGrantAndCannotDelegateAgain) {
  ModelServer model;
  model.Queue(AnthropicTool(
      "delegate-review", "delegate",
      R"({"tasks":[{"name":"review","instruction":"Inspect the project and report its name.","cards":[]}]})"));
  model.Queue(AnthropicTool("helper-inspect", "project_inspect", "{}"));
  model.Queue(
      AnthropicText("The authorized name is PRIVATE_PROJECT_SENTINEL."));
  model.Queue(AnthropicText("The helper completed the review."));
  Configure(model);
  StartProject();
  const auto conversation = Conversation("delegated-project-read");
  ASSERT_TRUE(Send(conversation, "Ask a helper to inspect the project.")
                  .Code()
                  .empty());
  ASSERT_TRUE(model.WaitForRequests(2));
  EXPECT_FALSE(model.WaitForRequests(3, 100ms));
  const auto grant = Grant(conversation);
  ASSERT_TRUE(model.WaitForRequests(4));
  EXPECT_NE(
      WaitUntilIdle(conversation).find("The helper completed the review."),
      std::string::npos);
  const auto requests = model.Requests();
  EXPECT_EQ(requests[1].find("PRIVATE_PROJECT_SENTINEL"), std::string::npos);
  EXPECT_EQ(requests[1].find("\"name\":\"delegate\""), std::string::npos);
  EXPECT_NE(requests[2].find("PRIVATE_PROJECT_SENTINEL"), std::string::npos);
  EXPECT_NE(requests[3].find("PRIVATE_PROJECT_SENTINEL"), std::string::npos);
  Revoke(grant);
  EXPECT_EQ(Get(conversation).Code(), "permission_denied");
  EXPECT_EQ(Send(conversation, "Continue the helper's work.", "turn-2").Code(),
            "permission_denied");
}

TEST_F(AgentRPC, OversizedDelegationsAreRefusedBeforeStartingAnyHelper) {
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

TEST_F(AgentRPC, CancellingAHelperPreservesItsTraceWithoutReplayingItOnResume) {
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

TEST_F(AgentRPC, AProjectIsNotSilentlyAddedToTheModelPrompt) {
  ModelServer model;
  model.Queue(AnthropicText("Only the explicit question was needed."));
  Configure(model);
  StartProject();
  const auto conversation = Conversation("no-implicit-read");
  ASSERT_TRUE(
      Send(conversation, "Explain what you can do before reading my project.")
          .Code()
          .empty());
  ASSERT_TRUE(model.WaitForRequests(1));
  WaitUntilIdle(conversation);
  const auto requests = model.Requests();
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(requests[0].find("PRIVATE_PROJECT_SENTINEL"), std::string::npos);
  EXPECT_EQ(Inspect(conversation).Code(), "grant_required");
}

TEST_F(AgentRPC,
       CustomSkillContentsNeedPermissionBeforeEnteringTheModelPrompt) {
  ModelServer model;
  model.Queue(AnthropicText("I received the authorized skill."));
  Configure(model);
  Write(directory.path / "project/skills/private.txt",
        "PRIVATE_SKILL_SENTINEL\nFollow the user's selected skill.");
  StartProject();
  const auto conversation = Conversation("custom-skill-permission");
  auto selected_skill = [&] {
    return Call(
        *external, A::RequestPayload::ConversationSendRequest, [&](Builder& b) {
          const auto skill = A::CreateSkillReference(
              b, b.CreateString("private-skill"), b.CreateString(project_id),
              b.CreateString("skills/private.txt"));
          const auto skills = b.CreateVector(std::vector{skill});
          return A::CreateConversationSendRequest(
                     b, b.CreateString(conversation),
                     b.CreateString("selected-skill"),
                     b.CreateString("Use my selected skill."), 0, 0, 0, 0, 0,
                     skills)
              .Union();
        });
  };
  ASSERT_TRUE(selected_skill().Code().empty());
  EXPECT_FALSE(model.WaitForRequests(1, 100ms));
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (Events(conversation).Json().find("permission") == std::string::npos &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(10ms);
  ASSERT_NE(Events(conversation).Json().find("permission"), std::string::npos);
  external.reset();
  human.reset();
  agent.Stop(true);
  StartAgent();
  const auto interrupted = Get(conversation).Json();
  EXPECT_TRUE(JsonBool(interrupted, "/resumable"));
  EXPECT_TRUE(JsonBool(interrupted, "/continuable"));
  ASSERT_TRUE(Call(*external, A::RequestPayload::ConversationActionRequest,
                   [&](Builder& b) {
                     return A::CreateConversationActionRequest(
                                b, b.CreateString(conversation),
                                A::Action::Resume)
                         .Union();
                   })
                  .Code()
                  .empty());
  EXPECT_FALSE(model.WaitForRequests(1, 100ms));
  const auto grant = Grant(conversation);
  ASSERT_TRUE(model.WaitForRequests(1));
  const auto completed = WaitUntilIdle(conversation);
  EXPECT_FALSE(JsonBool(completed, "/resumable"));
  EXPECT_FALSE(JsonBool(completed, "/continuable"));
  EXPECT_EQ(model.Requests().size(), 1u);
  EXPECT_NE(model.Requests()[0].find("PRIVATE_SKILL_SENTINEL"),
            std::string::npos);
  Revoke(grant);
  EXPECT_EQ(Send(conversation, "Continue with that skill.", "turn-2").Code(),
            "permission_denied");
}

TEST_F(AgentRPC, TheBundledDemoSkillCanBeUsedWithoutReadingAProject) {
  ModelServer model;
  model.Queue(AnthropicText("The bundled tour is available."));
  Configure(model);
  StartProject();
  const auto conversation = Conversation("bundled-demo-skill");
  const auto sent = Call(
      *external, A::RequestPayload::ConversationSendRequest, [&](Builder& b) {
        const auto names =
            b.CreateVectorOfStrings(std::vector<std::string>{"demo"});
        return A::CreateConversationSendRequest(
                   b, b.CreateString(conversation), b.CreateString("tour"),
                   b.CreateString("Show the bundled tour."), 0, 0, names)
            .Union();
      });
  ASSERT_TRUE(sent.Code().empty());
  ASSERT_TRUE(model.WaitForRequests(1));
  WaitUntilIdle(conversation);
  const auto request = model.Requests()[0];
  EXPECT_NE(request.find("User-selected bundled demo skill"),
            std::string::npos);
  EXPECT_EQ(request.find("PRIVATE_PROJECT_SENTINEL"), std::string::npos);
  EXPECT_EQ(Inspect(conversation).Code(), "grant_required");
}

TEST_F(AgentRPC, AuthorizedProjectImagesReachTheModelAsImages) {
  ModelServer model;
  model.Queue(AnthropicTool("read-image", "project_file_read",
                            R"({"path":"definitions/picture/src/pixel.png"})"));
  model.Queue(AnthropicText("I received the project image."));
  Configure(model);
  StartImageProject(true);
  const auto conversation = Conversation("project-image");
  Grant(conversation);
  ASSERT_TRUE(Send(conversation, "Read the picture.").Code().empty());
  ASSERT_TRUE(model.WaitForRequests(2));
  const auto state = WaitUntilIdle(conversation);
  ASSERT_NE(state.find("I received the project image."), std::string::npos);
  const auto requests = model.Requests();
  EXPECT_EQ(requests[0].find("iVBORw0KGgo"), std::string::npos);
  EXPECT_NE(requests[1].find("\"type\":\"image\""), std::string::npos);
  EXPECT_NE(requests[1].find("\"media_type\":\"image/png\""),
            std::string::npos);
  EXPECT_NE(requests[1].find("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAAD"
                             "UlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg=="),
            std::string::npos);
  EXPECT_NE(requests[1].find("shown with this result"), std::string::npos);
}

TEST_F(AgentRPC, CompletedModelMutationsAreReplayedWithoutExecutingAgain) {
  ModelServer model;
  Configure(model);
  StartProject();
  const auto conversation = Conversation("durable-model-write");
  const auto read_grant = Grant(conversation);
  Grant(conversation, A::GrantScope::Conversation, A::Access::Write);
  const auto original = JsonString(Inspect(conversation).Json(), "/text");
  const auto initial_revision = JsonString(original, "/value/rev");
  const auto document =
      R"({"name":"RENAMED_BY_MODEL","pkgsPath":"tsugumi:default","databaseConnections":[],"canvas":{"cards":[],"connections":[]}})";
  const auto arguments =
      "{\"base_rev\":" + Quote(initial_revision) +
      ",\"changes\":[{\"value_type\":\"SetProject\",\"value\":{\"json\":" +
      Quote(document) + "}}]}";
  model.Queue(AnthropicTool("model-save", "project_save", arguments));
  model.Queue(AnthropicText("The project was renamed."));
  ASSERT_TRUE(Send(conversation, "Rename the project.").Code().empty());
  ASSERT_TRUE(model.WaitForRequests(2));
  WaitUntilIdle(conversation);
  const auto saved = JsonString(Inspect(conversation).Json(), "/text");
  ASSERT_NE(saved.find("RENAMED_BY_MODEL"), std::string::npos);
  const auto revision = JsonString(saved, "/value/rev");
  EXPECT_EQ(std::stoull(revision), std::stoull(initial_revision) + 1);
  auto replay = [&](const std::string& input) {
    return Call(*external, A::RequestPayload::ToolCallRequest, [&](Builder& b) {
      return A::CreateToolCallRequest(
                 b, b.CreateString(conversation), b.CreateString(project_id),
                 b.CreateString("project_save"), b.CreateString(input),
                 b.CreateString("model-save"))
          .Union();
    });
  };
  const auto receipt = replay(arguments).Json();
  EXPECT_FALSE(JsonBool(receipt, "/isError"));
  external.reset();
  human.reset();
  agent.Stop();
  StartAgent();
  EXPECT_EQ(replay(arguments).Json(), receipt);
  EXPECT_EQ(JsonString(JsonString(Inspect(conversation).Json(), "/text"),
                       "/value/rev"),
            revision);
  auto changed = arguments;
  changed.replace(changed.find("RENAMED_BY_MODEL"),
                  std::string("RENAMED_BY_MODEL").size(), "SECOND_MUTATION");
  EXPECT_EQ(replay(changed).Code(), "conflict");
  Revoke(read_grant);
  EXPECT_EQ(replay(arguments).Code(), "grant_required");
}

TEST_F(AgentRPC, AnIncompleteProjectImageIsDescribedButNeverSentAsAnImage) {
  ModelServer model;
  model.Queue(AnthropicTool("read-incomplete-image", "project_file_read",
                            R"({"path":"definitions/picture/src/pixel.png"})"));
  model.Queue(AnthropicText("The picture was incomplete."));
  Configure(model);
  StartImageProject(false);
  const auto conversation = Conversation("incomplete-project-image");
  Grant(conversation);
  ASSERT_TRUE(Send(conversation, "Read the picture.").Code().empty());
  ASSERT_TRUE(model.WaitForRequests(2));
  WaitUntilIdle(conversation);
  const auto request = model.Requests()[1];
  EXPECT_EQ(request.find("\"type\":\"image\""), std::string::npos);
  EXPECT_NE(request.find("incomplete or invalid image"), std::string::npos);
  EXPECT_EQ(request.find("iVBORw0KGgo"), std::string::npos);
}

TEST_F(AgentRPC, AModelThatRejectsImagesCanContinueWithTheToolText) {
  ModelServer model;
  model.Queue(AnthropicTool("read-rejected-image", "project_file_read",
                            R"({"path":"definitions/picture/src/pixel.png"})"));
  model.QueueHTTP(R"({"error":{"message":"This model cannot accept images"}})",
                  400);
  model.Queue(AnthropicText("I continued with the image description."));
  Configure(model);
  StartImageProject(true);
  const auto conversation = Conversation("model-rejects-image");
  Grant(conversation);
  ASSERT_TRUE(Send(conversation, "Read the picture.").Code().empty());
  ASSERT_TRUE(model.WaitForRequests(3));
  EXPECT_NE(WaitUntilIdle(conversation)
                .find("I continued with the image description."),
            std::string::npos);
  const auto requests = model.Requests();
  EXPECT_NE(requests[1].find("\"type\":\"image\""), std::string::npos);
  EXPECT_EQ(requests[2].find("\"type\":\"image\""), std::string::npos);
  EXPECT_NE(requests[2].find("model rejected this image"), std::string::npos);
  EXPECT_NE(Events(conversation).Json().find("retrying"), std::string::npos);
}

TEST_F(AgentRPC,
       AModelToolCallWaitsForPermissionBeforeReceivingProjectContent) {
  ModelServer model;
  model.Queue(AnthropicTool("model-inspect", "project_inspect", "{}"));
  model.Queue(AnthropicText("I inspected the authorized project."));
  Configure(model);
  StartProject();
  const auto conversation = Conversation("model-permission");
  ASSERT_TRUE(Send(conversation, "Inspect the project after asking me.")
                  .Code()
                  .empty());
  ASSERT_TRUE(model.WaitForRequests(1));
  EXPECT_EQ(model.Requests().front().find("PRIVATE_PROJECT_SENTINEL"),
            std::string::npos);
  EXPECT_FALSE(model.WaitForRequests(2, 100ms));
  Grant(conversation);
  ASSERT_TRUE(model.WaitForRequests(2));
  EXPECT_NE(
      WaitUntilIdle(conversation).find("I inspected the authorized project."),
      std::string::npos);
  EXPECT_NE(model.Requests()[1].find("PRIVATE_PROJECT_SENTINEL"),
            std::string::npos);
}

TEST_F(AgentRPC, RevokedProjectContextCannotBeResentOrReadThroughOldEvents) {
  ModelServer model;
  model.Queue(AnthropicTool("read-private", "project_inspect", "{}"));
  model.Queue(AnthropicText("The private name is PRIVATE_PROJECT_SENTINEL."));
  Configure(model);
  StartProject();
  const auto conversation = Conversation("revocation");
  const auto grant = Grant(conversation);
  ASSERT_TRUE(
      Send(conversation, "Inspect the authorized project.").Code().empty());
  ASSERT_TRUE(model.WaitForRequests(2));
  WaitUntilIdle(conversation);
  Revoke(grant);
  EXPECT_EQ(Send(conversation, "Continue.", "turn-2").Code(),
            "permission_denied");
  EXPECT_EQ(Get(conversation).Code(), "permission_denied");
  EXPECT_EQ(Events(conversation).Code(), "permission_denied");
  EXPECT_EQ(model.Requests().size(), 2u);
  EXPECT_TRUE(Call(*human, A::RequestPayload::ConversationActionRequest,
                   [&](Builder& b) {
                     return A::CreateConversationActionRequest(
                                b, b.CreateString(conversation),
                                A::Action::Reset)
                         .Union();
                   })
                  .Code()
                  .empty());
  // Resetting model context must not expose previously recorded private events.
  const auto old_events = Events(conversation);
  if (old_events.Code().empty())
    EXPECT_EQ(old_events.Json().find("PRIVATE_PROJECT_SENTINEL"),
              std::string::npos);
  else
    EXPECT_EQ(old_events.Code(), "permission_denied");
}

TEST_F(AgentRPC, ConcurrentPluginToolsDoNotBlockTheUsersApproval) {
  ModelServer model;
  for (int i = 0; i < 4; ++i)
    model.Queue(AnthropicTool("inspect", "project_inspect", "{}"));
  for (int i = 0; i < 4; ++i)
    model.Queue(AnthropicText("The authorized project was inspected."));
  Configure(model);
  StartProject();
  const auto conversation = Conversation("concurrent-plugin-approval");
  std::vector<std::future<Reply>> tools;
  for (int i = 0; i < 4; ++i) {
    tools.push_back(std::async(std::launch::async, [&, i] {
      return Tool(
          conversation, "delegate",
          R"({"tasks":[{"name":"inspect","instruction":"Inspect the selected project."}]})",
          "parallel-tool-" + std::to_string(i));
    }));
  }
  ASSERT_TRUE(model.WaitForRequests(4));
  // All tool calls are waiting, but the user's control channel stays usable.
  const auto approval = Grant(conversation);
  EXPECT_FALSE(approval.empty());
  for (auto& tool : tools) {
    ASSERT_EQ(tool.wait_for(8s), std::future_status::ready);
    EXPECT_TRUE(tool.get().Code().empty());
  }
  EXPECT_EQ(model.Requests().size(), 8u);
}

}  // namespace
}  // namespace tsugumi::test
