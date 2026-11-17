#include <gtest/gtest.h>

#include <algorithm>

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
TEST_F(ToolArguments, RetryTextPreservesTheActualProviderError) {
  model.QueueHTTP(
      R"({"error":{"code":"overloaded_error","message":"Capacity exhausted"}})",
      503);
  model.Queue(AnthropicText("Recovered."));
  std::string retry;
  hooks.event = [&](const auto &event) {
    if (event.kind == "retrying") retry = event.text;
  };
  const auto result =
      agent::Runtime().Run(config, "system", messages, {}, {}, hooks);
  EXPECT_EQ(result.outcome, agent::RunOutcome::Finished) << result.error;
  EXPECT_NE(retry.find("Capacity exhausted"), std::string::npos);
  EXPECT_NE(retry.find("overloaded_error"), std::string::npos);
  EXPECT_NE(retry.find("retry 1"), std::string::npos);
}
TEST_F(ToolArguments, HttpErrorsLogTheReasonWithoutEchoedContextOrCredentials) {
  config.credential = "API_KEY_SENTINEL";
  config.auth = agent::ProviderAuth::ApiKey;
  agent::ContentBlock question;
  question.text = "MESSAGE_SENTINEL";
  agent::ContentBlock call;
  call.type = "tool_use";
  call.id = "call-one";
  call.name = "inspect";
  call.input = R"({"name":"ARGUMENT_SENTINEL"})";
  messages.push_back(
      {"assistant", {call}, {}, {}, agent::ProviderAPI::AnthropicMessages});
  agent::ContentBlock result;
  result.type = "tool_result";
  result.id = "call-one";
  result.text = "RESULT_SENTINEL";
  messages.push_back({"user",
                      {question, result},
                      {},
                      {},
                      agent::ProviderAPI::AnthropicMessages});
  model.QueueHTTP(
      R"({"detail":"Region unavailable API_KEY_SENTINEL INSTRUCTION_SENTINEL MESSAGE_SENTINEL RESULT_SENTINEL ARGUMENT_SENTINEL"})",
      403);
  testing::internal::CaptureStderr();
  try {
    agent::StreamReply(config, "INSTRUCTION_SENTINEL", messages, {}, {}, {});
    ADD_FAILURE() << "Refused model call was accepted";
  } catch (const agent::ProviderError &error) {
    EXPECT_EQ(error.status, 403);
    EXPECT_FALSE(error.retryable);
    EXPECT_NE(std::string(error.what()).find("Region unavailable"),
              std::string::npos);
  }
  const auto log = testing::internal::GetCapturedStderr();
  EXPECT_NE(log.find("Region unavailable"), std::string::npos);
  EXPECT_NE(log.find("http_status=403 curl_code=0"), std::string::npos);
  for (const auto *secret :
       {"API_KEY_SENTINEL", "INSTRUCTION_SENTINEL", "MESSAGE_SENTINEL",
        "RESULT_SENTINEL", "ARGUMENT_SENTINEL"})
    EXPECT_EQ(log.find(secret), std::string::npos) << log;
}
TEST_F(ToolArguments, NullableErrorCodeKeepsTheActualReason) {
  model.QueueHTTP(
      R"({"error":{"code":null,"type":"invalid_request_error","message":"This model is unavailable"}})",
      400);
  try {
    agent::StreamReply(config, "system", messages, {}, {}, {});
    FAIL() << "Refused model call was accepted";
  } catch (const agent::ProviderError &error) {
    EXPECT_EQ(error.code, "invalid_request_error");
    EXPECT_NE(std::string(error.what()).find("This model is unavailable"),
              std::string::npos);
  }
}
TEST_F(ToolArguments, MissingTerminalCallLogsOnlyStreamMetadata) {
  config.api = agent::ProviderAPI::OpenAIResponses;
  config.auth = agent::ProviderAuth::ApiKey;
  config.credential = "API_KEY_SENTINEL";
  const auto event = [](const std::string &json) {
    return "data: " + json + "\n\n";
  };
  model.Queue(
      event(
          R"({"type":"response.output_item.added","output_index":0,"item":{"type":"function_call","name":"inspect","namespace":"tsugumi","call_id":"call-one","arguments":""}})") +
      event(
          R"({"type":"response.function_call_arguments.delta","output_index":0,"delta":"ARGUMENT_SENTINEL"})") +
      event(
          R"({"type":"response.output_item.done","output_index":0,"item":{"type":"function_call","name":"inspect","namespace":"tsugumi","call_id":"call-one","arguments":"ARGUMENT_SENTINEL"}})") +
      event(
          R"({"type":"response.output_text.delta","output_index":1,"delta":"OUTPUT_SENTINEL"})") +
      event(R"({"type":"response.completed","response":{"output":[]}})"));
  agent::ContentBlock question;
  question.text = "MESSAGE_SENTINEL";
  messages.push_back(
      {"user", {question}, {}, {}, agent::ProviderAPI::OpenAIResponses});
  testing::internal::CaptureStderr();
  EXPECT_THROW(
      agent::StreamReply(config, "INSTRUCTION_SENTINEL", messages, {}, {}, {}),
      agent::ProviderError);
  const auto log = testing::internal::GetCapturedStderr();
  for (const auto *field :
       {"Model call failed api=responses auth=api_key", "http_status=200",
        "curl_code=0", "response.output_item.added", "response.completed",
        "\"streamed_calls\":1", "\"completed_calls\":0",
        "\"terminal_output\":true", "\"terminal_output_length\":0",
        "\"namespace\":\"tsugumi\"", "call-one", "inspect"})
    EXPECT_NE(log.find(field), std::string::npos) << field << " in " << log;
  for (const auto *secret :
       {"API_KEY_SENTINEL", "ARGUMENT_SENTINEL", "OUTPUT_SENTINEL",
        "MESSAGE_SENTINEL", "INSTRUCTION_SENTINEL"})
    EXPECT_EQ(log.find(secret), std::string::npos) << log;
  EXPECT_EQ(std::count(log.begin(), log.end(), '\n'), 1);
}
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
