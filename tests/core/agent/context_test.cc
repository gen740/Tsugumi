#include "agent/context.hh"

#include <gtest/gtest.h>

#include "agent/helpers.hh"
#include "agent/json.hh"
#include "util/text.hh"

namespace tsugumi::agent {
namespace {
TEST(AgentContext, ResultCapKeepsBothEndsAndLeavesTheRecordUntouched) {
  std::string text = "HEAD";
  for (int i = 0; i < 15000; ++i) text += "実験";
  text += "TAIL";
  ContentBlock call;
  call.type = "tool_use";
  call.id = "inspect-1";
  call.name = "project_inspect";
  ContentBlock result;
  result.type = "tool_result";
  result.id = call.id;
  result.text = text;
  const std::vector<Message> messages{
      {"assistant", {call}, {}, {}, ProviderAPI::AnthropicMessages},
      {"user", {result}, {}, {}, ProviderAPI::AnthropicMessages}};
  const auto view = RequestMessages(messages, 0);
  const auto& clipped = view.back().content[0].text;
  EXPECT_LE(clipped.size(), kToolResultBytes);
  EXPECT_TRUE(clipped.starts_with("HEAD"));
  EXPECT_TRUE(clipped.ends_with("TAIL"));
  EXPECT_TRUE(text::valid_utf8(clipped));
  EXPECT_NE(clipped.find("Omitted bytes ["), std::string::npos);
  EXPECT_NE(clipped.find("tool_result_read"), std::string::npos);
  EXPECT_NE(clipped.find("inspect-1"), std::string::npos);
  EXPECT_EQ(messages.back().content[0].text, text);
  const auto omitted = RequestMessages(messages, 2);
  EXPECT_NE(omitted.back().content[0].text.find("project_inspect {}"),
            std::string::npos);
  EXPECT_EQ(omitted.front().content[0].input, call.input);
}

TEST(AgentContext, HelperAlwaysReturnsItsLastReportWithoutTheTranscript) {
  ContentBlock text;
  text.text = "earlier private detail";
  std::vector<Message> trace{
      {"user", {text}, {}, {}, ProviderAPI::AnthropicMessages}};
  text.text = "First answer";
  trace.push_back(
      {"assistant", {text}, {}, {}, ProviderAPI::AnthropicMessages});
  text.text = "Final report";
  trace.push_back(
      {"assistant", {text}, {}, {}, ProviderAPI::AnthropicMessages});
  auto result = json::Object(Helpers::HelperResult(
      json::Encode(json::Members{{"name", json::Quote("review")},
                                 {"messages", SerializeMessages(trace)}}),
      "conversation", "delegation", 2));
  EXPECT_EQ(json::Text(result, "report"), "Final report");
  EXPECT_FALSE(result.contains("messages"));
  EXPECT_EQ(json::Text(result, "call_id"), "delegation");
  EXPECT_EQ(json::Get(result, "task"), "2");
  text.text = std::string(20000, 'x') + "END";
  trace.back().content = {text};
  result = json::Object(Helpers::HelperResult(
      json::Encode(json::Members{{"messages", SerializeMessages(trace)}}),
      "conversation", "delegation", 2));
  EXPECT_TRUE(json::Bool(json::Get(result, "shortened")));
  EXPECT_LE(json::Text(result, "report").size(), 8192u);
  EXPECT_NE(json::Text(result, "report").find("conversation/delegation/2"),
            std::string::npos);
}
}  // namespace
}  // namespace tsugumi::agent
