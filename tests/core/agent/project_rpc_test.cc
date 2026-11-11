// Scenarios exercise only the public Agent RPC through the shipped client.
#include "project_fixture.hh"

#include <unistd.h>

namespace tsugumi::test {
namespace {

TEST_F(AgentProjectFixture, ReadingAProjectRequiresAnExplicitUserGrant) {
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

TEST_F(AgentProjectFixture,
       LookingUpAnOperationPreservesItsIdAndRequiresOnlyReadAccess) {
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

TEST_F(AgentProjectFixture,
       ClearingProjectCacheUsesItsPublicRequestWithoutAddingOrigin) {
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

TEST_F(AgentProjectFixture,
       SavingAViewRequiresWritePermissionEvenWithoutAnOperationId) {
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

TEST_F(AgentProjectFixture,
       ConversationGrantsDoNotLeakIntoAnotherSessionAndCanBeRevoked) {
  StartProject();
  const auto first = Conversation("first");
  const auto second = Conversation("second");
  const auto grant = Grant(first);
  EXPECT_TRUE(Inspect(first).Code().empty());
  EXPECT_EQ(Inspect(second).Code(), "grant_required");
  Revoke(grant);
  EXPECT_EQ(Inspect(first).Code(), "grant_required");
}

TEST_F(AgentProjectFixture,
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

TEST_F(AgentProjectFixture,
       ConversationAndApprovedAccessSurviveAnAgentServerRestart) {
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

TEST_F(AgentProjectFixture,
       AContextRefusalShrinksToolResultsBeforeAskingAgain) {
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

TEST_F(AgentProjectFixture,
       DelegatedWorkUsesTheParentGrantAndCannotDelegateAgain) {
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

TEST_F(AgentProjectFixture, AProjectIsNotSilentlyAddedToTheModelPrompt) {
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

TEST_F(AgentProjectFixture,
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

TEST_F(AgentProjectFixture,
       TheBundledDemoSkillCanBeUsedWithoutReadingAProject) {
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

TEST_F(AgentProjectFixture, AuthorizedProjectImagesReachTheModelAsImages) {
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

TEST_F(AgentProjectFixture,
       CompletedModelMutationsAreReplayedWithoutExecutingAgain) {
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

TEST_F(AgentProjectFixture,
       AnIncompleteProjectImageIsDescribedButNeverSentAsAnImage) {
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

TEST_F(AgentProjectFixture, AModelThatRejectsImagesCanContinueWithTheToolText) {
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

TEST_F(AgentProjectFixture,
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

TEST_F(AgentProjectFixture,
       RevokedProjectContextCannotBeResentOrReadThroughOldEvents) {
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

TEST_F(AgentProjectFixture, ConcurrentPluginToolsDoNotBlockTheUsersApproval) {
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

TEST_F(AgentProjectFixture, ShellCommandsRequireTheirOwnExplicitGrant) {
  if (::geteuid() == 0) GTEST_SKIP() << "Shell commands require an unprivileged project user";
  StartProject(true);
  const auto conversation = Conversation("shell-permission");
  for (auto access : {A::Access::Read, A::Access::Write, A::Access::Run})
    Grant(conversation, A::GrantScope::Conversation, access);
  const auto denied =
      Tool(conversation, "project_shell", R"({"command":"printf allowed"})");
  EXPECT_EQ(denied.Code(), "grant_required");
  EXPECT_NE(Events(conversation).Json().find("Shell"), std::string::npos);
  Grant(conversation, A::GrantScope::Conversation, A::Access::Shell);
  const auto result =
      Tool(conversation, "project_shell",
           R"({"command":"printf allowed; printf diagnostic >&2; exit 3"})");
  ASSERT_TRUE(result.Code().empty());
  const auto text = JsonString(result.Json(), "/text");
  EXPECT_EQ(JsonString(text, "/reply"), "ProjectShellResult");
  EXPECT_EQ(JsonString(text, "/value/stdout"), "allowed");
  EXPECT_EQ(JsonString(text, "/value/stderr"), "diagnostic");
  EXPECT_NE(text.find("\"exit_code\":3"), std::string::npos);
}

TEST_F(AgentProjectFixture, ModelShellCommandsAndResultsAppearInToolHistory) {
  if (::geteuid() == 0) GTEST_SKIP() << "Shell commands require an unprivileged project user";
  StartProject(true);
  const auto conversation = Conversation("shell-history");
  for (auto access :
       {A::Access::Read, A::Access::Write, A::Access::Run, A::Access::Shell})
    Grant(conversation, A::GrantScope::Conversation, access);
  ModelServer model;
  model.Queue(AnthropicTool("shell", "project_shell",
                            R"({"command":"printf shell-history-output"})"));
  model.Queue(AnthropicText("The command finished."));
  Configure(model);
  ASSERT_TRUE(Send(conversation, "Run the command.").Code().empty());
  EXPECT_NE(WaitUntilIdle(conversation).find("The command finished."),
            std::string::npos);
  const auto events = Events(conversation).Json();
  EXPECT_NE(events.find("toolStarted"), std::string::npos);
  EXPECT_NE(events.find("printf shell-history-output"), std::string::npos);
  EXPECT_NE(events.find("shell-history-output"), std::string::npos);
  ASSERT_EQ(model.Requests().size(), 2u);
  EXPECT_NE(model.Requests()[1].find("ProjectShellResult"), std::string::npos);
}

TEST_F(AgentProjectFixture, CancellingTheConversationStopsItsShellCommand) {
  if (::geteuid() == 0) GTEST_SKIP() << "Shell commands require an unprivileged project user";
  StartProject(true);
  const auto conversation = Conversation("shell-cancel");
  for (auto access :
       {A::Access::Read, A::Access::Write, A::Access::Run, A::Access::Shell})
    Grant(conversation, A::GrantScope::Conversation, access);
  ModelServer model;
  model.Queue(AnthropicTool(
      "shell", "project_shell",
      R"({"command":"printf ready > shell-ready; sleep 2; printf escaped > shell-escaped","timeout_ms":10000})"));
  Configure(model);
  ASSERT_TRUE(Send(conversation, "Start a command, then wait.").Code().empty());
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (!std::filesystem::exists(directory.path / "project/shell-ready") &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(10ms);
  ASSERT_TRUE(std::filesystem::exists(directory.path / "project/shell-ready"));
  ASSERT_TRUE(Call(*external, A::RequestPayload::ConversationCancelRequest,
                   [&](Builder& b) {
                     return A::CreateConversationCancelRequest(
                                b, b.CreateString(conversation))
                         .Union();
                   })
                  .Code()
                  .empty());
  WaitUntilIdle(conversation);
  std::this_thread::sleep_for(2100ms);
  EXPECT_FALSE(
      std::filesystem::exists(directory.path / "project/shell-escaped"));
  EXPECT_NE(Events(conversation).Json().find("cancelled"), std::string::npos);
}

TEST_F(AgentProjectFixture,
       ExternalShellCallsHaveVisibleHistoryAndCanBeCancelled) {
  if (::geteuid() == 0) GTEST_SKIP() << "Shell commands require an unprivileged project user";
  StartProject(true);
  const auto conversation = Conversation("external-shell-cancel");
  for (auto access :
       {A::Access::Read, A::Access::Write, A::Access::Run, A::Access::Shell})
    Grant(conversation, A::GrantScope::Conversation, access);
  auto pending = std::async(std::launch::async, [&] {
    return Tool(
        conversation, "project_shell",
        R"({"command":"printf ready > external-ready; sleep 1; printf escaped > external-escaped"})");
  });
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (!std::filesystem::exists(directory.path / "project/external-ready") &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(10ms);
  ASSERT_TRUE(
      std::filesystem::exists(directory.path / "project/external-ready"));
  ASSERT_TRUE(Call(*human, A::RequestPayload::ConversationCancelRequest,
                   [&](Builder& b) {
                     return A::CreateConversationCancelRequest(
                                b, b.CreateString(conversation))
                         .Union();
                   })
                  .Code()
                  .empty());
  ASSERT_EQ(pending.wait_for(3s), std::future_status::ready);
  EXPECT_EQ(pending.get().Code(), "cancelled");
  std::this_thread::sleep_for(1100ms);
  EXPECT_FALSE(
      std::filesystem::exists(directory.path / "project/external-escaped"));
  const auto events = Events(conversation).Json();
  EXPECT_NE(events.find("toolStarted"), std::string::npos);
  EXPECT_NE(events.find("toolFinished"), std::string::npos);
  EXPECT_NE(events.find("printf ready > external-ready"), std::string::npos);
}

TEST_F(AgentProjectFixture, RevokingShellAccessStopsAnExternalCommand) {
  if (::geteuid() == 0) GTEST_SKIP() << "Shell commands require an unprivileged project user";
  StartProject(true);
  const auto conversation = Conversation("external-shell-revoke");
  for (auto access : {A::Access::Read, A::Access::Write, A::Access::Run})
    Grant(conversation, A::GrantScope::Conversation, access);
  const auto grant =
      Grant(conversation, A::GrantScope::Conversation, A::Access::Shell);
  auto pending = std::async(std::launch::async, [&] {
    return Tool(
        conversation, "project_shell",
        R"({"command":"printf ready > revoke-ready; sleep 1; printf escaped > revoke-escaped"})");
  });
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (!std::filesystem::exists(directory.path / "project/revoke-ready") &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(10ms);
  ASSERT_TRUE(std::filesystem::exists(directory.path / "project/revoke-ready"));
  Revoke(grant);
  ASSERT_EQ(pending.wait_for(3s), std::future_status::ready);
  EXPECT_EQ(pending.get().Code(), "cancelled");
  std::this_thread::sleep_for(1100ms);
  EXPECT_FALSE(
      std::filesystem::exists(directory.path / "project/revoke-escaped"));
  EXPECT_EQ(Tool(conversation, "project_shell", R"({"command":"true"})").Code(),
            "grant_required");
}

}  // namespace
}  // namespace tsugumi::test
