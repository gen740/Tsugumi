// The same scenario runs against the baseline and candidate installed core.
#include <gtest/gtest.h>

#include <algorithm>
#include <iostream>
#include <numeric>
#include <thread>

#include "../agent_fixture_server.hh"
#include "agent/runtime.hh"
#include "agent/store.hh"
#include "host/store.hh"

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
  int summaries = 0;
  std::vector<size_t> input_sizes;
  std::jthread scripted([&](std::stop_token stop) {
    size_t next = 1;
    while (!stop.stop_requested()) {
      if (!model.WaitForRequests(next, std::chrono::milliseconds(100)))
        continue;
      const auto request = model.Requests()[next++ - 1];
      if (request.find("Summarize the quoted earlier conversation") !=
              std::string::npos ||
          request.find(
              "Create a short handoff for the next model continuation") !=
              std::string::npos) {
        ++summaries;
        model.Queue(AnthropicText(
            "Want: inspect all results. Decisions: read only. Done: earlier "
            "inspections. Failures: none. Left: continue.",
            0));
      } else {
        input_sizes.push_back(request.size());
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
  ASSERT_EQ(input_sizes.size(), kCalls + 1);
  const auto human = store.ConversationJson(store.GetConversation("measure"));
  for (int i = 1; i <= kCalls; ++i) {
    const auto id = "inspect-" + std::to_string(i);
    const auto text = id + " HEAD " + std::string(48000, 'x') + " TAIL " + id;
    EXPECT_NE(human.find(text), std::string::npos) << id;
  }
  // bd315031's real-runtime measurement: retain its send-size improvement
  // while fixing overflow recovery. These are wire bytes, not model tokens.
  const auto total =
      std::accumulate(input_sizes.begin(), input_sizes.end(), size_t{0});
  const auto maximum =
      *std::max_element(input_sizes.begin(), input_sizes.end());
  EXPECT_LE(total, 3839651u);
  EXPECT_LE(maximum, 137774u);
  EXPECT_EQ(summaries, 0);
  std::cout << "CONTEXT_MEASURE normal_request_bytes=[";
  for (size_t i = 0; i < input_sizes.size(); ++i)
    std::cout << (i ? "," : "") << input_sizes[i];
  std::cout << "] compactions=" << summaries << " visible_results=" << kCalls
            << " total_bytes=" << total << " max_bytes=" << maximum << '\n';
}
}  // namespace
}  // namespace tsugumi::test
