#include <gtest/gtest.h>

#include "../agent_fixture_server.hh"
#include "agent/context.hh"
#include "agent/json.hh"
#include "agent/runtime.hh"
#include "util/crypto.hh"

namespace tsugumi::test {
namespace {
TEST(FileResults, KeepTheirStructureInActualProviderRequests) {
  namespace agent = tsugumi::agent;
  namespace json = agent::json;
  ModelServer model;
  model.Queue(AnthropicTool("shell", "project_shell", "{}"));
  model.Queue(AnthropicTool("read", "project_file_read", "{}"));
  model.Queue(AnthropicTool("binary", "attached_folder_read", "{}"));
  model.Queue(AnthropicText("done"));
  const std::string output =
      "OUTPUT_HEAD" + std::string(262100, '\1') + "OUTPUT_TAIL";
  std::vector<std::string> changed_files(
      3000, json::Encode(json::Members{{"path", Quote(std::string(100, 'p'))},
                                       {"size_bytes", Quote("3")}}));
  const std::string binary_output =
      "BINARY_HEAD" + std::string(262100, '\xff') + "BINARY_TAIL";
  const auto shell = json::Encode(json::Members{
      {"reply", Quote("ProjectShellResult")},
      {"value", json::Encode(json::Members{
                    {"stdout_bytes",
                     json::Encode(json::Members{
                         {"base64", Quote(crypto::base64(binary_output))}})},
                    {"stderr_bytes", Quote(output)},
                    {"stdout_size", Quote("900000")},
                    {"stderr_size", Quote("900000")},
                    {"exit_code", "0"},
                    {"stdout_truncated", "true"},
                    {"stderr_truncated", "true"},
                    {"changed_files", json::Encode(changed_files)},
                    {"save", R"({"rev":"1","delta":{"origin":"agent"}})"}})}});
  auto read = [&](bool binary) {
    return json::Encode(json::Members{
        {"reply", Quote("FileContent")},
        {"value",
         json::Encode(json::Members{
             {"bytes", binary ? json::Encode(json::Members{
                                    {"base64", Quote(crypto::base64(std::string(
                                                   262144, '\xff')))}})
                              : Quote(std::string(262144, '"'))},
             {"offset", Quote("32")},
             {"next_offset", Quote("262176")},
             {"size_bytes", Quote("262176")},
             {"eof", "true"}})}});
  };
  agent::RuntimeHooks hooks;
  hooks.authorize_context = [](const auto&) {};
  hooks.checkpoint = [](const auto&) {};
  hooks.execute_tool = [&](const agent::ToolCall& call, std::stop_token) {
    return agent::ToolResult{
        call.id == "shell" ? shell : read(call.id == "binary"), false, {}, {}};
  };
  agent::ProviderConfig config;
  config.auth = agent::ProviderAuth::None;
  config.base_url = model.URL();
  config.model = "fixture";
  agent::ContentBlock instruction;
  instruction.text = "Exercise file tool results";
  std::vector<agent::Message> messages{
      {"user", {instruction}, {}, {}, agent::ProviderAPI::AnthropicMessages}};
  const auto result =
      agent::Runtime().Run(config, "file results", messages,
                           {{"project_shell", "shell", "{}"},
                            {"project_file_read", "read", "{}"},
                            {"attached_folder_read", "read", "{}"}},
                           {}, hooks);
  ASSERT_EQ(result.outcome, agent::RunOutcome::Finished) << result.error;
  const auto requests = model.Requests();
  ASSERT_EQ(requests.size(), 4u);
  const auto model_shell =
      JsonString(requests[1], "/messages/2/content/0/content");
  ASSERT_LE(model_shell.size(), agent::kToolResultBytes);
  EXPECT_EQ(model_shell.find("Omitted bytes"), std::string::npos);
  EXPECT_NE(JsonString(model_shell, "/value/hint").find("tool_result_read"),
            std::string::npos);
  EXPECT_NE(JsonString(model_shell, "/value/hint").find("shell"),
            std::string::npos);
  EXPECT_EQ(messages[2].content[0].text, shell);
  EXPECT_EQ(JsonString(model_shell, "/reply"), "ProjectShellResult");
  EXPECT_EQ(JsonUInt(model_shell, "/value/changed_files_count"), 3000u);
  EXPECT_EQ(JsonString(model_shell, "/value/save/rev"), "1");
  for (const auto* stream : {"stdout", "stderr"}) {
    std::string value;
    if (std::string_view(stream) == "stdout") {
      ASSERT_TRUE(crypto::base64_decode(
          JsonString(model_shell, "/value/stdout_bytes/base64"), value));
      EXPECT_TRUE(value.starts_with("BINARY_HEAD"));
      EXPECT_TRUE(value.ends_with("BINARY_TAIL"));
    } else {
      value = JsonString(model_shell, "/value/stderr_bytes");
      EXPECT_TRUE(value.starts_with("OUTPUT_HEAD"));
      EXPECT_TRUE(value.ends_with("OUTPUT_TAIL"));
    }
    EXPECT_EQ(
        JsonString(model_shell, std::string("/value/") + stream + "_size"),
        "900000");
  }
  // Overflow recovery reshapes the original record once at the smaller cap.
  for (const auto cap : {agent::kToolResultBytes / 2, size_t{1024}}) {
    const auto view = agent::RequestMessages(messages, 0, cap);
    for (size_t i : {2u, 4u, 6u}) {
      const auto& shaped = view[i].content[0].text;
      ASSERT_LE(shaped.size(), cap);
      EXPECT_EQ(shaped.find("Omitted bytes"), std::string::npos);
      EXPECT_FALSE(JsonString(shaped, "/reply").empty());
    }
  }
  for (size_t i = 1; i <= 2; ++i) {
    const auto page =
        JsonString(requests[i + 1], "/messages/" + std::to_string(2 * (i + 1)) +
                                        "/content/0/content");
    ASSERT_LE(page.size(), agent::kToolResultBytes);
    EXPECT_EQ(page.find("Omitted bytes"), std::string::npos);
    EXPECT_NE(JsonString(page, "/value/hint").find("next_offset"),
              std::string::npos);
    EXPECT_EQ(JsonString(page, "/reply"), "FileContent");
    const auto value = json::Object(json::Get(json::Object(page), "value"));
    auto data = json::Text(value, "bytes");
    if (i == 2)
      ASSERT_TRUE(crypto::base64_decode(
          json::Text(json::Object(value.at("bytes")), "base64"), data));
    EXPECT_EQ(JsonString(page, "/value/next_offset"),
              std::to_string(32 + data.size()));
    EXPECT_EQ(JsonString(page, "/value/size_bytes"), "262176");
    EXPECT_FALSE(JsonBool(page, "/value/eof"));
  }
}

}  // namespace
}  // namespace tsugumi::test
