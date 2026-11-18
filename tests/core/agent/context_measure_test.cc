// The same scenario runs against the baseline and candidate installed core.
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <iostream>
#include <numeric>
#include <thread>

#include "../agent_fixture_server.hh"
#include "agent/runtime.hh"
#include "agent/store.hh"
#include "host/store.hh"
#include "tsugumi/util/json_object.hh"

namespace tsugumi::test {
namespace {
TEST(AgentContextMeasure, LongToolConversationKeepsEveryVisibleResult) {
  ModelServer model;
  TemporaryDirectory directory;
  host::StateGate gate;
  host::HostStore home(directory.path);
  host::ProjectRegistry registry(gate, home.db, [](const auto&) {
    return std::optional<std::filesystem::path>{};
  });
  agent::ConversationStore store(
      gate, home.db, registry, [](std::string text) { return text; },
      [](const auto&, const auto&, auto) { return true; },
      [](const auto&) { return false; });
  flatbuffers::FlatBufferBuilder builder;
  Tsugumi::Agent::ConversationCreateRequestT create;
  create.conversation_id = "measure";
  builder.Finish(
      Tsugumi::Agent::ConversationCreateRequest::Pack(builder, &create));
  store.Create(*flatbuffers::GetRoot<Tsugumi::Agent::ConversationCreateRequest>(
                   builder.GetBufferPointer()),
               true);
  constexpr int kCalls = 40;
  int calls = 0;
  std::atomic<bool> compacting{false};
  std::jthread scripted([&](std::stop_token stop) {
    size_t next = 1;
    while (!stop.stop_requested()) {
      if (!model.WaitForRequests(next, stop)) break;
      const auto request = model.Requests()[next++ - 1];
      if (compacting.exchange(false)) {
        model.Queue(AnthropicText(
            "Want: inspect all results. Decisions: read only. Done: earlier "
            "inspections. Failures: none. Left: continue.",
            0));
      } else {
        if (calls < kCalls) {
          auto stream = AnthropicTool("inspect-" + std::to_string(++calls),
                                      "inspect", "{}");
          const auto marker = std::string("\"input_tokens\":12");
          stream.replace(stream.find(marker), marker.size(),
                         "\"input_tokens\":" + std::to_string(request.size()));
          model.Queue(std::move(stream));
        } else
          model.Queue(
              AnthropicText("All inspections are recorded.", request.size()));
      }
    }
  });
  agent::ProviderConfig config;
  config.auth = agent::ProviderAuth::None;
  config.base_url = model.URL();
  config.model = "fixture";
  config.context_limit_tokens = 200000;
  agent::RuntimeHooks hooks;
  hooks.event = [&](const auto& event) {
    if (event.kind == "compacting") compacting.store(true);
  };
  hooks.authorize_context = [](const auto&) {};
  hooks.checkpoint = [&](const auto& saved) {
    store.SaveMessages("measure", saved);
  };
  hooks.execute_tool = [](const auto& call, auto) {
    return agent::ToolResult{
        call.id + " HEAD " + std::string(48000, 'x') + " TAIL " + call.id,
        false,
        {},
        {}};
  };
  std::vector<agent::Message> messages;
  agent::ContentBlock instruction;
  instruction.text = "Inspect forty records, keeping all evidence.";
  messages.push_back(
      {"user", {instruction}, {}, {}, agent::ProviderAPI::AnthropicMessages});
  const auto result = agent::Runtime().Run(
      config, "system", messages, {{"inspect", "Inspect one record.", "{}"}},
      {}, hooks);
  scripted.request_stop();
  scripted.join();
  ASSERT_EQ(result.outcome, agent::RunOutcome::Finished) << result.error;
  ASSERT_EQ(calls, kCalls);
  const auto human = store.ConversationData(store.GetConversation("measure"));
  for (int i = 1; i <= kCalls; ++i) {
    const auto id = "inspect-" + std::to_string(i);
    const auto text = id + " HEAD " + std::string(48000, 'x') + " TAIL " + id;
    const auto contains = [&](const auto& messages) {
      return std::ranges::any_of(messages, [&](const auto& message) {
        return message->content_json.find(text) != std::string::npos;
      });
    };
    EXPECT_TRUE(contains(human.messages) || contains(human.archive)) << id;
  }
}
}  // namespace
}  // namespace tsugumi::test
