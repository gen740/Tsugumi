#include <gtest/gtest.h>

#include "../agent_fixture_server.hh"
#include "agent/catalog.hh"
#include "agent/runtime.hh"
#include "host/failure.hh"

namespace tsugumi::agent {
TEST(AgentCatalog, SchemaMistakesNameTheFieldAndTheEditingTool) {
  const Catalog catalog;
  const auto wrong =
      R"({"base_rev":"1","changes":[{"file_path":"definitions/X/src/main.py","old_string":"a","new_string":"b"}]})";
  for (const auto validate_only : {false, true}) {
    try {
      if (validate_only)
        catalog.ValidateArguments("project_save", wrong);
      else
        (void)catalog.Request("project_save", wrong, 1);
      FAIL() << "Wrong tool shape was accepted";
    } catch (const host::Failure &error) {
      EXPECT_EQ(error.code, "invalid_request");
      const std::string diagnostic = error.what();
      for (const auto *text :
           {"file_path", "project_save", "value_type", "project_edit"})
        EXPECT_NE(diagnostic.find(text), std::string::npos) << diagnostic;
      EXPECT_EQ(diagnostic.find('\n'), std::string::npos);
    }
  }
  EXPECT_NO_THROW(catalog.ValidateArguments(
      "project_edit",
      R"({"edits":[{"file_path":"a","old_string":"b","new_string":"c"}]})"));
  try {
    catalog.ValidateArguments(
        "project_edit",
        R"({"edits":[{"file_path":"a","old_string":"b","new_string":"c","z_misspelled":true}]})");
    FAIL() << "Unknown argument was ignored";
  } catch (const host::Failure &error) {
    EXPECT_EQ(error.code, "invalid_request");
    EXPECT_NE(std::string(error.what()).find("z_misspelled"),
              std::string::npos);
    EXPECT_NE(std::string(error.what()).find("project_edit"),
              std::string::npos);
  }
}
} // namespace tsugumi::agent

namespace tsugumi::test {
namespace {
class ToolArguments : public testing::Test {
protected:
  ModelServer model;
  agent::ProviderConfig config;
  agent::ContextUsage context;
  std::vector<agent::Message> messages;
  agent::RuntimeHooks hooks;
  ToolArguments() {
    config.base_url = model.URL();
    config.auth = agent::ProviderAuth::None;
    config.model = "fixture";
    config.context_limit_tokens = 200000;
    hooks.authorize_context = [](const auto &) {};
    hooks.checkpoint = [](const auto &) {};
  }
};
TEST_F(ToolArguments, MalformedToolArgumentsReportReceivedBytesAndOutputLimit) {
  config.max_tokens = 4321;
  const std::string arguments = "{\"edits\":[";
  model.Queue(AnthropicTool("bad-arguments", "project_edit", arguments), false,
              true);
  model.Queue(AnthropicText("Send a smaller corrected edit."));
  int executions = 0;
  hooks.execute_tool = [&](const auto &, auto) {
    ++executions;
    return agent::ToolResult{};
  };
  agent::ContentBlock question;
  question.text = "Edit the Card.";
  messages.push_back(
      {"user", {question}, {}, {}, agent::ProviderAPI::AnthropicMessages});
  const auto result = agent::Runtime().Run(
      config, "system", messages,
      {{"project_edit", "edit", R"({"type":"object"})"}}, {}, hooks, &context);
  EXPECT_EQ(result.outcome, agent::RunOutcome::Finished) << result.error;
  EXPECT_EQ(executions, 0);
  ASSERT_EQ(model.Requests().size(), 2u);
  const auto retry = model.Requests()[1];
  for (const auto &expected : std::vector<std::string>{
           "project_edit", std::to_string(arguments.size()),
           "4321", "tool_use"})
    EXPECT_NE(retry.find(expected), std::string::npos) << retry;
  EXPECT_NE(messages[1].content[0].input, "{}");
  EXPECT_EQ(messages[1].content[0].input, arguments);
}

TEST_F(ToolArguments, OutputLimitReportsArgumentsWithoutExecutingAnyCall) {
  config.max_tokens = 2345;
  const std::string arguments = "{\"edits\":[";
  auto limited = AnthropicTool("cut-arguments", "project_edit", arguments);
  const std::string reason = "\"stop_reason\":\"tool_use\"";
  limited.replace(limited.find(reason), reason.size(),
                  "\"stop_reason\":\"max_tokens\"");
  model.Queue(limited);
  model.Queue(AnthropicText("Continue with fewer edits."));
  int executions = 0;
  hooks.execute_tool = [&](const auto &, auto) {
    ++executions;
    return agent::ToolResult{};
  };
  const auto result = agent::Runtime().Run(
      config, "system", messages,
      {{"project_edit", "edit", R"({"type":"object"})"}}, {}, hooks, &context);
  EXPECT_EQ(result.outcome, agent::RunOutcome::Finished) << result.error;
  EXPECT_EQ(executions, 0);
  ASSERT_EQ(model.Requests().size(), 2u);
  const auto retry = model.Requests()[1];
  for (const auto &expected :
       {std::string("2345"),
        "project_edit=" + std::to_string(arguments.size())})
    EXPECT_NE(retry.find(expected), std::string::npos) << retry;
  EXPECT_EQ(retry.find("\"type\":\"tool_use\""), std::string::npos);
}

} // namespace
} // namespace tsugumi::test
