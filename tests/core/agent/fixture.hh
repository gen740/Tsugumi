#pragma once

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

#include "../agent_fixture_server.hh"
#include "tsugumi/client/client.hh"
#include "tsugumi/protocol/agent_generated.h"

namespace tsugumi::test {

namespace A = Tsugumi::Agent;
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

inline auto Call(client::Connection& connection, A::RequestPayload type,
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

class AgentFixture : public ::testing::Test {
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
  std::string ready;
  std::string project_id;
  std::unique_ptr<client::Connection> human;
  std::unique_ptr<client::Connection> external;
  uint64_t operation_id = 0;
};

}  // namespace tsugumi::test
