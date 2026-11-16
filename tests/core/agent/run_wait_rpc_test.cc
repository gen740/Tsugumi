#include "project_fixture.hh"

namespace tsugumi::test {
namespace {
class AgentRunWaitFixture : public AgentProjectFixture {
protected:
  auto Prepare() -> std::string {
    StartProject();
    const auto conversation = Conversation("run-wait");
    for (auto access :
         {Tsugumi::Service::Access::Read, Tsugumi::Service::Access::Write,
          Tsugumi::Service::Access::Run})
      Grant(conversation, Tsugumi::Service::GrantScope::Conversation, access);
    const auto inspected = Inspect(conversation).View();
    const auto rev = JsonString(JsonString(inspected, "/text"), "/value/rev");
    const auto saved = Tool(conversation, "project_save",
                            "{\"base_rev\":" + Quote(rev) + R"(,"changes":[
      {"value_type":"SetProject","value":{"json":"{\"name\":\"wait\",\"pkgsPath\":\"tsugumi:default\",\"canvas\":{\"cards\":[{\"id\":\"work\"}],\"connections\":[]}}"}},
      {"value_type":"PutDefinition","value":{"id":"work","card_json":"{\"language\":\"bash\"}","meta_json":"{\"description\":\"Wait fixture\",\"context\":\"Run completion\"}"}},
      {"value_type":"PutSource","value":{"path":"definitions/work/_outputs.fbs","bytes":"table Outputs {} root_type Outputs;"}},
      {"value_type":"PutSource","value":{"path":"definitions/work/default.nix","bytes":"{}"}},
      {"value_type":"PutSource","value":{"path":"definitions/work/mock-build.json","bytes":"{\"gate\":\"run-wait\",\"error\":\"fixture completion\"}"}}
    ]})");
    if (!saved.Code().empty() || JsonBool(saved.View(), "/isError"))
      throw std::runtime_error("Cannot save wait fixture: " + saved.View());
    return conversation;
  }

  auto StartRun(const std::string &conversation, int wait = 0,
                std::string operation = {}) -> Reply {
    const auto snapshot = Inspect(conversation).View();
    const auto revision =
        JsonString(JsonString(snapshot, "/text"), "/value/rev");
    return Tool(conversation, "run",
                "{\"base_rev\":" + Quote(revision) +
                    ",\"card\":{\"segments\":[\"work\"]},\"wait_seconds\":" +
                    std::to_string(wait) + "}",
                operation);
  }
  auto RunId(const Reply &reply) -> std::string {
    if (!reply.Code().empty())
      throw std::runtime_error(reply.View());
    return JsonString(JsonString(reply.View(), "/text"),
                      "/value/run/run_id");
  }
  auto WaitArguments(const std::string &run, int seconds) -> std::string {
    return "{\"run_id\":" + Quote(run) +
           ",\"wait_seconds\":" + std::to_string(seconds) + "}";
  }
  auto Entered() -> void {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!std::filesystem::exists(directory.path / "mock/run-wait.started")) {
      if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("Run did not prepare: " +
                                 Read(directory.path / "project.log"));
      std::this_thread::sleep_for(10ms);
    }
  }
  auto Release() -> void {
    Write(directory.path / "mock/run-wait.release", "ready");
  }
  auto ToolStarted(const std::string &conversation) -> void {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (Events(conversation).View().find("run_status") ==
           std::string::npos) {
      if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("Wait tool did not start");
      std::this_thread::sleep_for(10ms);
    }
  }
  auto Stop(const std::string &conversation) -> Reply {
    return Call(*human, A::RequestPayload::ConversationCancelRequest,
                [&](Builder &b) {
                  return A::CreateConversationCancelRequest(
                             b, b.CreateString(conversation))
                      .Union();
                });
  }
};

TEST_F(AgentRunWaitFixture, StatusReturnsEarlyWhenTheRunEnds) {
  const auto conversation = Prepare();
  const auto run = RunId(StartRun(conversation));
  Entered();
  const auto begin = std::chrono::steady_clock::now();
  auto pending = std::async(std::launch::async, [&] {
    return Tool(conversation, "run_status", WaitArguments(run, 300));
  });
  EXPECT_EQ(pending.wait_for(100ms), std::future_status::timeout);
  Release();
  ASSERT_EQ(pending.wait_for(3s), std::future_status::ready);
  const auto reply = pending.get();
  ASSERT_TRUE(reply.Code().empty()) << reply.View();
  const auto text = JsonString(reply.View(), "/text");
  EXPECT_EQ(JsonString(text, "/value/state"), "failed");
  EXPECT_FALSE(JsonBool(text, "/still_running"));
  EXPECT_EQ(JsonString(text, "/wait_reason"), "ended");
  EXPECT_LT(std::chrono::steady_clock::now() - begin, 4s);
}

TEST_F(AgentRunWaitFixture,
       TimeoutReturnsCurrentStateAndZeroReturnsImmediately) {
  const auto conversation = Prepare();
  const auto run = RunId(StartRun(conversation));
  Entered();
  for (int seconds : {0, 1}) {
    const auto begin = std::chrono::steady_clock::now();
    const auto reply =
        Tool(conversation, "run_status", WaitArguments(run, seconds));
    ASSERT_TRUE(reply.Code().empty()) << reply.View();
    const auto text = JsonString(reply.View(), "/text");
    EXPECT_EQ(JsonString(text, "/value/state"), "preparing");
    if (seconds) {
      EXPECT_GE(std::chrono::steady_clock::now() - begin, 900ms);
      EXPECT_TRUE(JsonBool(text, "/still_running"));
      EXPECT_EQ(JsonString(text, "/wait_reason"), "timeout");
    } else {
      EXPECT_LT(std::chrono::steady_clock::now() - begin, 1s);
    }
  }
  Release();
}

TEST_F(AgentRunWaitFixture, RunStartUsesTheSameWaitAndStopDoesNotCancelTheRun) {
  const auto conversation = Prepare();
  auto pending = std::async(std::launch::async, [&] {
    return StartRun(conversation, 300, "start-wait");
  });
  Entered();
  EXPECT_EQ(pending.wait_for(100ms), std::future_status::timeout);
  ASSERT_TRUE(Stop(conversation).Code().empty());
  ASSERT_EQ(pending.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(pending.get().Code(), "cancelled");
  const auto current = Tool(conversation, "run_status", "{}");
  EXPECT_EQ(JsonString(JsonString(current.View(), "/text"), "/value/state"),
            "preparing");
  Release();
  const auto recovered = StartRun(conversation, 300, "start-wait");
  ASSERT_TRUE(recovered.Code().empty()) << recovered.View();
  const auto recovered_text = JsonString(recovered.View(), "/text");
  EXPECT_EQ(JsonString(recovered_text, "/wait_reason"), "ended");
  EXPECT_EQ(RunId(recovered),
            JsonString(JsonString(current.View(), "/text"), "/value/run_id"));
}

TEST_F(AgentRunWaitFixture,
       RunStartReturnsAtTimeoutAndCanThenBeWaitedToCompletion) {
  const auto conversation = Prepare();
  const auto started = StartRun(conversation, 1);
  ASSERT_TRUE(started.Code().empty()) << started.View();
  const auto text = JsonString(started.View(), "/text");
  EXPECT_TRUE(JsonBool(text, "/still_running"));
  EXPECT_EQ(JsonString(text, "/wait_reason"), "timeout");
  const auto run = RunId(started);
  Release();
  const auto ended = Tool(conversation, "run_status", WaitArguments(run, 300));
  ASSERT_TRUE(ended.Code().empty()) << ended.View();
  EXPECT_EQ(JsonString(JsonString(ended.View(), "/text"), "/wait_reason"),
            "ended");
}

TEST_F(AgentRunWaitFixture, StoppingTheConversationInterruptsAStatusWait) {
  const auto conversation = Prepare();
  const auto run = RunId(StartRun(conversation));
  Entered();
  auto pending = std::async(std::launch::async, [&] {
    return Tool(conversation, "run_status", WaitArguments(run, 300));
  });
  ToolStarted(conversation);
  EXPECT_EQ(pending.wait_for(100ms), std::future_status::timeout);
  ASSERT_TRUE(Stop(conversation).Code().empty());
  ASSERT_EQ(pending.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(pending.get().Code(), "cancelled");
  Release();
}

TEST_F(AgentRunWaitFixture, CoreExitEndsTheWaitWithAClearFailure) {
  const auto conversation = Prepare();
  const auto run = RunId(StartRun(conversation));
  Entered();
  auto pending = std::async(std::launch::async, [&] {
    return Tool(conversation, "run_status", WaitArguments(run, 300));
  });
  EXPECT_EQ(pending.wait_for(100ms), std::future_status::timeout);
  project.Stop(true);
  ASSERT_EQ(pending.wait_for(2s), std::future_status::ready);
  const auto reply = pending.get();
  EXPECT_EQ(reply.Code(), "unavailable");
  ASSERT_NE(reply.Failure(), nullptr);
  EXPECT_NE(reply.Failure()->message()->str().find("connection"),
            std::string::npos);
}

TEST_F(AgentRunWaitFixture,
       ModelWaitIsOneToolCallAndDoesNotBlockOtherConversations) {
  const auto conversation = Prepare();
  const auto run = RunId(StartRun(conversation));
  Entered();
  ModelServer model;
  model.Queue(
      AnthropicTool("wait-model", "run_status", WaitArguments(run, 300)));
  model.Queue(AnthropicText("Run ended."));
  Configure(model);
  ASSERT_TRUE(Send(conversation, "Wait for the result.").Code().empty());
  ASSERT_TRUE(model.WaitForRequests(1));
  ToolStarted(conversation);
  const auto other = Conversation("other-conversation");
  Grant(other);
  ASSERT_TRUE(Inspect(other).Code().empty());
  EXPECT_TRUE(JsonBool(Get(conversation).View(), "/busy"));
  EXPECT_EQ(model.Requests().size(), 1u);
  Release();
  WaitUntilIdle(conversation);
  ASSERT_EQ(model.Requests().size(), 2u);
  const auto result =
      JsonString(model.Requests()[1], "/messages/2/content/0/content");
  EXPECT_EQ(JsonString(result, "/wait_reason"), "ended");
  EXPECT_FALSE(JsonBool(result, "/still_running"));
}

TEST_F(AgentRunWaitFixture, StopInterruptsAModelWaitWithoutAnotherModelStep) {
  const auto conversation = Prepare();
  const auto run = RunId(StartRun(conversation));
  Entered();
  ModelServer model;
  model.Queue(
      AnthropicTool("wait-stop", "run_status", WaitArguments(run, 300)));
  Configure(model);
  ASSERT_TRUE(Send(conversation, "Wait for this run.").Code().empty());
  ASSERT_TRUE(model.WaitForRequests(1));
  ToolStarted(conversation);
  ASSERT_TRUE(Stop(conversation).Code().empty());
  WaitUntilIdle(conversation);
  EXPECT_EQ(model.Requests().size(), 1u);
  EXPECT_NE(Events(conversation).View().find("cancelled"), std::string::npos);
  Release();
}

TEST_F(AgentRunWaitFixture,
       IdleWaitReturnsImmediatelyAndInvalidTimeoutsAreRefused) {
  const auto conversation = Prepare();
  const auto reply =
      Tool(conversation, "run_status", R"({"wait_seconds":999})");
  ASSERT_TRUE(reply.Code().empty());
  const auto text = JsonString(reply.View(), "/text");
  EXPECT_EQ(JsonString(text, "/wait_reason"), "idle");
  EXPECT_FALSE(JsonBool(text, "/still_running"));
  for (const auto *timeout : {"-1", "1.5", "\"invalid\""}) {
    EXPECT_EQ(Tool(conversation, "run_status",
                   std::string("{\"wait_seconds\":") + timeout + "}")
                  .Code(),
              "invalid_request");
  }
}

TEST_F(AgentRunWaitFixture, HelperCanStartAndWaitWithinItsRunScope) {
  const auto conversation = Prepare();
  const auto revision = JsonString(
      JsonString(Inspect(conversation).View(), "/text"), "/value/rev");
  ModelServer model;
  model.Queue(AnthropicTool(
      "helper-run", "run",
      "{\"base_rev\":" + Quote(revision) +
          ",\"card\":{\"segments\":[\"work\"]},\"wait_seconds\":300}"));
  model.Queue(AnthropicText("The run ended."));
  Configure(model);
  auto pending = std::async(std::launch::async, [&] {
    return Tool(
        conversation, "delegate",
        R"({"tasks":[{"name":"runner","instruction":"Run work and wait for its result.","read_cards":["work"],"write_cards":[],"run_cards":["work"]}]})");
  });
  Entered();
  EXPECT_EQ(pending.wait_for(100ms), std::future_status::timeout);
  Release();
  ASSERT_EQ(pending.wait_for(3s), std::future_status::ready);
  const auto reply = pending.get();
  ASSERT_TRUE(reply.Code().empty()) << reply.View();
  ASSERT_EQ(model.Requests().size(), 2u);
  const auto result =
      JsonString(model.Requests()[1], "/messages/2/content/0/content");
  EXPECT_FALSE(JsonBool(model.Requests()[1], "/messages/2/content/0/is_error"));
  EXPECT_EQ(JsonString(result, "/value/run/state"), "failed");
  EXPECT_FALSE(JsonBool(result, "/still_running"));
}

TEST_F(AgentRunWaitFixture, HelperCannotWaitOnAnUnownedRun) {
  const auto conversation = Prepare();
  const auto run = RunId(StartRun(conversation));
  Entered();
  ModelServer model;
  model.Queue(
      AnthropicTool("helper-wait", "run_status", WaitArguments(run, 300)));
  model.Queue(AnthropicText("Run is outside my scope."));
  Configure(model);
  const auto delegated = Tool(
      conversation, "delegate",
      R"({"tasks":[{"name":"scoped","instruction":"Wait for the given run.","read_cards":["work"],"write_cards":[],"run_cards":["work"]}]})");
  ASSERT_TRUE(delegated.Code().empty()) << delegated.View();
  ASSERT_EQ(model.Requests().size(), 2u);
  EXPECT_TRUE(JsonBool(model.Requests()[1], "/messages/2/content/0/is_error"));
  EXPECT_NE(JsonString(model.Requests()[1], "/messages/2/content/0/content")
                .find("unowned run"),
            std::string::npos);
  Release();
}
} // namespace
} // namespace tsugumi::test
