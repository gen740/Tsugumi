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
#include "view.hh"

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
  auto HostValue() const -> const H::Reply * {
    return H::GetMessage(bytes.data())->payload_as_Reply();
  }
  auto Failure() const -> const Tsugumi::Service::Failure * {
    return H::MessageBufferHasIdentifier(bytes.data())
               ? HostValue()->value_as_Failure()
               : Value()->value_as_Failure();
  }
  auto Code() const -> std::string {
    const auto *f = Failure();
    return f ? f->code()->str() : "";
  }
  auto RequestId() const -> std::string {
    const auto *f = Failure();
    return f && f->request_id() ? f->request_id()->str() : "";
  }
  auto View() const -> std::string {
    if (const auto *f = Failure())
      throw std::runtime_error(f->code()->str() + ": " + f->message()->str());
    if (H::MessageBufferHasIdentifier(bytes.data())) {
      const auto *r = HostValue();
      switch (r->value_type()) {
      case H::ReplyPayload::HelloResult:
        return test::View(r->value_as_HelloResult());
      case H::ReplyPayload::Acknowledgement:
        return test::View(r->value_as_Acknowledgement());
      case H::ReplyPayload::ProjectDefaults:
        return test::View(r->value_as_ProjectDefaults());
      case H::ReplyPayload::ProjectInfo:
        return test::View(r->value_as_ProjectInfo());
      case H::ReplyPayload::ProjectLocation:
        return test::View(r->value_as_ProjectLocation());
      case H::ReplyPayload::RecentProject:
        return test::View(r->value_as_RecentProject());
      case H::ReplyPayload::RecentList:
        return test::View(r->value_as_RecentList()->projects());
      case H::ReplyPayload::DemoList:
        return test::View(r->value_as_DemoList()->demos());
      case H::ReplyPayload::ModelList:
        return test::View(r->value_as_ModelList()->models());
      default:
        throw std::runtime_error("Unexpected typed Host reply");
      }
    }
    const auto *r = Value();
    switch (r->value_type()) {
    case A::ReplyPayload::HelloResult:
      return test::View(r->value_as_HelloResult());
    case A::ReplyPayload::Acknowledgement:
      return test::View(r->value_as_Acknowledgement());
    case A::ReplyPayload::ConversationResult:
      return test::View(r->value_as_ConversationResult());
    case A::ReplyPayload::ConversationList:
      return test::View(r->value_as_ConversationList()->conversations());
    case A::ReplyPayload::SearchResult:
      return test::View(r->value_as_SearchResult()->matches());
    case A::ReplyPayload::EventsPage:
      return test::View(r->value_as_EventsPage());
    case A::ReplyPayload::ToolCatalog:
      return test::View(r->value_as_ToolCatalog()->tools());
    case A::ReplyPayload::ToolResult:
      return test::View(r->value_as_ToolResult());
    case A::ReplyPayload::AccessResult:
      return test::View(r->value_as_AccessResult());
    case A::ReplyPayload::AccessList:
      return test::View(r->value_as_AccessList());
    case A::ReplyPayload::DemoList:
      return test::View(r->value_as_DemoList()->paths());
    case A::ReplyPayload::TextResult:
      return test::View(r->value_as_TextResult());
    default:
      throw std::runtime_error("Unexpected typed Agent reply");
    }
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

inline auto Call(client::Connection &connection, H::RequestPayload type,
                 const Build &build) -> Reply {
  auto result =
      connection
          .Call(
              [&](uint64_t id) {
                Builder b;
                const auto value = build(b);
                const auto request = H::CreateRequest(b, type, value);
                H::FinishMessageBuffer(b, H::CreateMessage(b, id,
                                                           H::Payload::Request,
                                                           request.Union()));
                return client::Bytes(b.GetBufferPointer(),
                                     b.GetBufferPointer() + b.GetSize());
              },
              5s)
          .get();
  if (result.error)
    throw std::runtime_error("Host transport: " + result.error.message);
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
    host.reset();
    external.reset();
    human.reset();
    agent.Stop();
  }

  std::vector<std::string> extra_agent_arguments;

  auto StartAgent() -> void {
    std::filesystem::remove(directory.path / "state/agent.ready");
    std::vector<std::string> arguments{TSUGUMI_AGENT_BINARY, "--state-dir",
         (directory.path / "state").string(), "--unix",
         (directory.path / "agent.sock").string(), "--demos-dir",
         (directory.path / "demos").string(), "--listen", "127.0.0.1:0"};
    arguments.insert(arguments.end(), extra_agent_arguments.begin(), extra_agent_arguments.end());
    agent.Start(arguments, directory.path / "agent.log");
    ready = Readiness(agent, directory.path / "state/agent.ready",
                      directory.path / "agent.log");
    human = Connect(true);
    external = Connect(false);
    host = ConnectHost();
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
        Call(*connection, A::RequestPayload::HelloRequest, [&](Builder &b) {
          return Tsugumi::Service::CreateHelloRequest(
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

  auto ConnectHost(bool websocket = false, bool user = true,
                   bool accepted = true)
      -> std::unique_ptr<client::Connection> {
    client::Options options;
    options.protocol = client::Protocol::Host;
    options.endpoint = websocket ? JsonString(ready, "/host_endpoint")
                                 : "unix:" + JsonString(ready, "/host_socket");
    auto connection = std::make_unique<client::Connection>(options);
    if (auto error = connection->Start().get())
      throw std::runtime_error(error.message);
    const auto hello =
        Call(*connection, H::RequestPayload::HelloRequest, [&](Builder &b) {
          return Tsugumi::Service::CreateHelloRequest(
                     b,
                     b.CreateString(JsonString(
                         ready, user ? "/user_token" : "/external_token")),
                     b.CreateString("host-test"))
              .Union();
        });
    if (accepted != (hello.HostValue()->value_as_HelloResult() != nullptr))
      throw std::runtime_error("Unexpected Host authentication result");
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
    return JsonString(reply.View(), "/conversation/id");
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
    const auto id = JsonString(ask.View(), "/id");
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
    auto reply =
        Call(*host, H::RequestPayload::ProviderSetRequest, [&](Builder& b) {
          Tsugumi::Service::ProviderInputT config;
          config.api_key = "FIXTURE_API_KEY";
          config.endpoint = model.URL();
          config.model = "fixture-model";
          config.max_tokens = 1024;
          config.context_limit = context_limit;
          const auto p = Tsugumi::Service::ProviderInput::Pack(b, &config);
          return H::CreateProviderSetRequest(b, b.CreateString("desktop"),
                                             b.CreateString(provider), p)
              .Union();
        });
    if (!reply.Code().empty())
      throw std::runtime_error("cannot configure external model fixture");
    const auto current =
        Call(*host, H::RequestPayload::SettingsGetRequest, [](Builder& b) {
          return H::CreateSettingsGetRequest(b, b.CreateString("desktop"))
              .Union();
        });
    const auto revision = current.HostValue()->value_as_Settings()->revision();
    const auto selected =
        Call(*host, H::RequestPayload::SettingsUpdateRequest, [&](Builder& b) {
          const auto item =
              H::CreateSetting(b, b.CreateString("agent.provider"),
                               b.CreateString(Quote(provider)));
          return H::CreateSettingsUpdateRequest(
                     b, b.CreateString("desktop"), revision,
                     b.CreateVector(std::vector{item}))
              .Union();
        });
    if (!selected.Code().empty())
      throw std::runtime_error("cannot select fixture provider");
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
      auto state = Get(conversation, true).View();
      if (!JsonBool(state, "/busy")) return state;
      std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("Agent did not finish:\n" +
                             Events(conversation, true).View() + "\n" +
                             Read(directory.path / "agent.log"));
  }

  TemporaryDirectory directory;
  ChildProcess agent;
  std::string ready;
  std::string project_id;
  std::unique_ptr<client::Connection> host;
  std::unique_ptr<client::Connection> human;
  std::unique_ptr<client::Connection> external;
  uint64_t operation_id = 0;
};

}  // namespace tsugumi::test
