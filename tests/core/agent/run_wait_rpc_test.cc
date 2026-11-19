#include "project_fixture.hh"
#include "tsugumi/util/json_object.hh"

namespace tsugumi::test {
namespace {
TEST_F(AgentProjectFixture,
       MissingSavedInputsNamesTheWorkflowTriggerAndKeepsSingleCardRuns) {
  const auto root = directory.path / "project";
  Write(root / "definitions/load/_card.json", R"({"template":"trigger"})");
  Write(
      root / "definitions/source/_card.json",
      R"({"template":"input","params":[{"name":"value","type":"double","data":1}]})");
  Write(root / "definitions/source/_outputs.fbs",
        "table Value { value:double; } table Outputs { out:Value (required); } "
        "root_type Outputs;");
  Write(root / "definitions/knn/_card.json", R"({"template":"collect"})");
  Write(root / "definitions/nested/_link.json", R"({"external":"nested"})");
  Write(root / "externals/nested/tsugumi.json",
        R"({"canvas":{"cards":[{"id":"inner_load"}],"connections":[]}})");
  Write(root / "externals/nested/definitions/inner_load/_card.json",
        R"({"template":"trigger"})");
  StartProject(
      false,
      R"({"pkgsPath":"tsugumi:default","canvas":{"cards":[{"id":"load"},{"id":"source"},{"id":"knn"},{"id":"nested"}],"connections":[{"from":{"card":"source","port":"out"},"to":{"card":"knn","port":"in"}}]}})");
  const auto conversation = Conversation("missing-inputs");
  for (const auto access :
       {Tsugumi::Service::Access::Read, Tsugumi::Service::Access::Run})
    Grant(conversation, access);
  const auto revision = JsonString(
      JsonString(Inspect(conversation).View(), "/text"), "/value/rev");
  auto run = [&](const std::string& card, const std::string& id) {
    const auto reply =
        Tool(conversation, "run",
             "{\"base_rev\":" + Quote(revision) + ",\"card\":{\"segments\":[" +
                 Quote(card) + "]},\"propagate\":" +
                 (card == "load" ? "true" : "false") + ",\"wait_seconds\":10}",
             id);
    EXPECT_TRUE(reply.Code().empty());
    return JsonString(reply.View(), "/text");
  };
  const auto refused = run("knn", "without-inputs");
  EXPECT_EQ(JsonString(refused, "/value/run/state"), "failed") << refused;
  EXPECT_EQ(JsonString(refused, "/value/run/failure/kind"), "input_missing")
      << refused;
  EXPECT_NE(refused.find("load"), std::string::npos) << refused;
  EXPECT_NE(refused.find("nested/inner_load"), std::string::npos) << refused;
  const auto whole = run("load", "trigger");
  EXPECT_EQ(JsonString(whole, "/value/run/state"), "succeeded") << whole;
  const auto single = run("knn", "saved-inputs");
  EXPECT_EQ(JsonString(single, "/value/run/state"), "succeeded") << single;
}

class AgentRunWaitFixture : public AgentProjectFixture {
 protected:
  auto Prepare() -> std::string {
    StartProject();
    const auto conversation = Conversation("run-wait");
    for (auto access :
         {Tsugumi::Service::Access::Read, Tsugumi::Service::Access::Write,
          Tsugumi::Service::Access::Run})
      Grant(conversation, access);
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

  auto StartRun(const std::string& conversation, int wait = 0,
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
  auto RunId(const Reply& reply) -> std::string {
    if (!reply.Code().empty()) throw std::runtime_error(reply.View());
    return JsonString(JsonString(reply.View(), "/text"), "/value/run/run_id");
  }
  auto WaitArguments(const std::string& run, int seconds) -> std::string {
    return "{\"run_id\":" + Quote(run) +
           ",\"wait_seconds\":" + std::to_string(seconds) + "}";
  }
  auto Entered() -> void {
    const auto path = directory.path / "mock/run-wait.started";
    if (!WaitForPath(
            path, [&] { return std::filesystem::exists(path); }, project.Pid()))
      throw std::runtime_error("Project service exited: " +
                               Read(directory.path / "project.log"));
  }
  auto Release() -> void {
    Write(directory.path / "mock/run-wait.release", "ready");
  }
  auto ToolStarted(const std::string& conversation,
                   std::string_view selector = "run_status",
                   size_t required = 1) -> void {
    // External tool starts are available through the event history.
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (true) {
      const auto reply = Events(conversation);
      const auto* events = reply.Value()->value_as_EventsPage()->events();
      size_t started = 0;
      if (events)
        for (const auto* item : *events) {
          const auto* event = item->event();
          if (event->kind()->string_view() == "toolStarted" &&
              ((event->name() && event->name()->string_view() == selector) ||
               (event->id() && event->id()->string_view() == selector)))
            ++started;
        }
      if (started >= required) return;
      if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("Wait tool did not start");
      std::this_thread::sleep_for(10ms);
    }
  }
  auto Stop(const std::string& conversation) -> Reply {
    return Call(*human, A::RequestPayload::ConversationCancelRequest,
                [&](Builder& b) {
                  return A::CreateConversationCancelRequest(
                             b, b.CreateString(conversation))
                      .Union();
                });
  }
  auto PendingStatus(const std::string& conversation, const std::string& run,
                     const std::string& operation, uint64_t& request_id)
      -> std::future<client::Result> {
    return external->Call([&](uint64_t id) {
      request_id = id;
      Builder b;
      const auto value = A::CreateToolCallRequest(
          b, b.CreateString(conversation), b.CreateString(project_id),
          b.CreateString("run_status"), b.CreateString(WaitArguments(run, 300)),
          b.CreateString(operation));
      const auto request = A::CreateRequest(
          b, A::RequestPayload::ToolCallRequest, value.Union());
      A::FinishMessageBuffer(
          b, A::CreateMessage(b, id, A::Payload::Request, request.Union()));
      return client::Bytes(b.GetBufferPointer(),
                           b.GetBufferPointer() + b.GetSize());
    });
  }
  auto CancelCall(client::Connection& connection, uint64_t request_id)
      -> Reply {
    return Call(connection, A::RequestPayload::ToolCallCancelRequest,
                [&](Builder& b) {
                  return A::CreateToolCallCancelRequest(b, request_id).Union();
                });
  }
};

TEST_F(AgentRunWaitFixture,
       RequestCancellationStopsOnlyItsOwnConnectionAndCall) {
  const auto conversation = Prepare();
  const auto run = RunId(StartRun(conversation));
  Entered();
  uint64_t first_id = 0, second_id = 0;
  auto first = PendingStatus(conversation, run, "first-wait", first_id);
  auto second = PendingStatus(conversation, run, "second-wait", second_id);
  ToolStarted(conversation);
  ASSERT_TRUE(CancelCall(*human, first_id).Code().empty());
  ASSERT_TRUE(CancelCall(*external, first_id).Code().empty());
  auto reply = first.get();
  ASSERT_FALSE(reply.error) << reply.error.message;
  EXPECT_EQ((Reply{std::move(reply.bytes)}).Code(), "cancelled");
  EXPECT_TRUE(Inspect(conversation).Code().empty());
  Release();

  reply = second.get();
  ASSERT_FALSE(reply.error) << reply.error.message;
  EXPECT_TRUE((Reply{std::move(reply.bytes)}).Code().empty());
}

TEST_F(AgentRunWaitFixture, ConcurrentWaitsLeaveNormalToolsAvailable) {
  const auto conversation = Prepare();
  const auto run = RunId(StartRun(conversation));
  Entered();
  std::vector<std::future<client::Result>> waits;
  for (int i = 0; i < 6; ++i) {
    uint64_t id = 0;
    waits.push_back(PendingStatus(conversation, run,
                                  "parallel-wait-" + std::to_string(i), id));
  }
  // Occupy the dedicated wait slots before invoking an ordinary tool.
  ToolStarted(conversation, "run_status", 4);
  const auto other = Conversation("while-waiting");
  Grant(other);
  // Inspect replies through the normal RPC path while wait slots are occupied.
  EXPECT_TRUE(Inspect(other).Code().empty());
  EXPECT_TRUE(
      Tool(conversation, "run_cancel", "{\"run_id\":" + Quote(run) + "}")
          .Code()
          .empty());
  Release();
  for (auto& wait : waits) EXPECT_FALSE(wait.get().error);
}

TEST_F(AgentRunWaitFixture, DeletingAConversationStopsAndUnregistersItsWait) {
  const auto conversation = Prepare();
  const auto run = RunId(StartRun(conversation));
  Entered();
  auto pending = std::async(std::launch::async, [&] {
    return Tool(conversation, "run_status", WaitArguments(run, 300),
                "deleted-wait");
  });
  ToolStarted(conversation);
  ASSERT_TRUE(Call(*human, A::RequestPayload::ConversationDeleteRequest,
                   [&](Builder& b) {
                     return A::CreateConversationDeleteRequest(
                                b, b.CreateString(conversation))
                         .Union();
                   })
                  .Code()
                  .empty());
  EXPECT_EQ(pending.get().Code(), "cancelled");
  EXPECT_EQ(Conversation("run-wait"), conversation);
  Grant(conversation);
  // Reusing the deleted ID/call must not find a leaked registration or old
  // toolFinished event, and stopping a wait must leave the run active.
  EXPECT_EQ(Events(conversation).View().find("deleted-wait"),
            std::string::npos);
  const auto current =
      Tool(conversation, "run_status", WaitArguments(run, 0), "deleted-wait");
  ASSERT_TRUE(current.Code().empty());
  EXPECT_EQ(JsonString(JsonString(current.View(), "/text"), "/value/state"),
            "preparing");
  Release();
}

TEST_F(AgentRunWaitFixture, DisconnectingStopsAndUnregistersTheExternalWait) {
  const auto conversation = Prepare();
  const auto run = RunId(StartRun(conversation));
  Entered();
  uint64_t id = 0;
  auto pending = PendingStatus(conversation, run, "disconnect-wait", id);
  ToolStarted(conversation);
  external->Close();

  EXPECT_TRUE(pending.get().error);
  // Agent tool history is queried via EventsRequest without a push event.
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (Events(conversation, true).View().find("cancelled") ==
         std::string::npos) {
    ASSERT_LT(std::chrono::steady_clock::now(), deadline);
    std::this_thread::sleep_for(10ms);
  }
  external = Connect(false);
  const auto current = Tool(conversation, "run_status", WaitArguments(run, 0));
  ASSERT_TRUE(current.Code().empty()) << current.Code();
  EXPECT_EQ(JsonString(JsonString(current.View(), "/text"), "/value/state"),
            "preparing");
  Release();
  // Receipts survive a transport disconnect: a retry must keep the same
  // arguments, as well as the same call ID.
  const auto retried = Tool(conversation, "run_status", WaitArguments(run, 300),
                            "disconnect-wait");
  ASSERT_TRUE(retried.Code().empty()) << retried.Code();
  EXPECT_EQ(JsonString(JsonString(retried.View(), "/text"), "/wait_reason"),
            "ended");
}

TEST_F(AgentRunWaitFixture,
       StopInterruptsStartAndStatusWithoutCancellingTheRun) {
  const auto conversation = Prepare();
  auto pending = std::async(std::launch::async, [&] {
    return StartRun(conversation, 300, "start-wait");
  });
  Entered();
  const auto current = Tool(conversation, "run_status", "{}", "get-active-run");
  const auto status = JsonString(current.View(), "/text");
  EXPECT_EQ(JsonString(status, "/value/state"), "preparing");
  EXPECT_FALSE(::tsugumi::json::Object(status).contains("wait_reason"));
  const auto run = JsonString(status, "/value/run_id");
  auto status_wait = std::async(std::launch::async, [&] {
    return Tool(conversation, "run_status", WaitArguments(run, 300),
                "status-wait");
  });
  ToolStarted(conversation, "status-wait");
  ASSERT_TRUE(Stop(conversation).Code().empty());

  EXPECT_EQ(pending.get().Code(), "cancelled");
  EXPECT_EQ(status_wait.get().Code(), "cancelled");
  const auto after_stop =
      Tool(conversation, "run_status", WaitArguments(run, 0));
  const auto status_after_stop = JsonString(after_stop.View(), "/text");
  EXPECT_EQ(JsonString(status_after_stop, "/value/state"), "preparing");
  EXPECT_FALSE(
      ::tsugumi::json::Object(status_after_stop).contains("wait_reason"));
  Release();
  const auto recovered = StartRun(conversation, 300, "start-wait");
  ASSERT_TRUE(recovered.Code().empty()) << recovered.View();
  const auto recovered_text = JsonString(recovered.View(), "/text");
  EXPECT_EQ(JsonString(recovered_text, "/wait_reason"), "ended");
  EXPECT_EQ(RunId(recovered), run);
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
  Entered();
  auto pending = std::async(std::launch::async, [&] {
    return Tool(conversation, "run_status", WaitArguments(run, 300),
                "finish-wait");
  });
  ToolStarted(conversation, "finish-wait");
  Release();
  const auto ended = pending.get();
  ASSERT_TRUE(ended.Code().empty()) << ended.View();
  const auto final_status = JsonString(ended.View(), "/text");
  EXPECT_EQ(JsonString(final_status, "/value/state"), "failed");
  EXPECT_FALSE(JsonBool(final_status, "/still_running"));
  EXPECT_EQ(JsonString(final_status, "/wait_reason"), "ended");
}

TEST_F(AgentRunWaitFixture, CoreExitEndsTheWaitWithAClearFailure) {
  const auto conversation = Prepare();
  const auto run = RunId(StartRun(conversation));
  Entered();
  auto pending = std::async(std::launch::async, [&] {
    return Tool(conversation, "run_status", WaitArguments(run, 300));
  });
  ToolStarted(conversation);
  project.Stop(true);

  const auto reply = pending.get();
  EXPECT_EQ(reply.Code(), "unavailable");
  ASSERT_NE(reply.Failure(), nullptr);
}

TEST_F(AgentRunWaitFixture,
       ModelWaitIsOneToolCallAndDoesNotBlockOtherConversations) {
  const auto conversation = Prepare();
  const auto run = RunId(StartRun(conversation));
  Entered();
  ModelServer model;
  model.Queue(AnthropicFamilyTool("wait-model", "runs", "status",
                                  WaitArguments(run, 300)));
  model.Queue(AnthropicText("Run ended."));
  Configure(model);
  ASSERT_TRUE(Send(conversation, "Wait for the result.").Code().empty());
  ASSERT_TRUE(model.WaitForRequests(1));
  ToolStarted(conversation, "tools");
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
  model.Queue(AnthropicFamilyTool("wait-stop", "runs", "status",
                                  WaitArguments(run, 300)));
  Configure(model);
  ASSERT_TRUE(Send(conversation, "Wait for this run.").Code().empty());
  ASSERT_TRUE(model.WaitForRequests(1));
  ToolStarted(conversation, "tools");
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
  for (const auto* timeout : {"-1", "1.5", "\"invalid\""}) {
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
  Release();

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
  model.Queue(AnthropicFamilyTool("helper-wait", "runs", "status",
                                  WaitArguments(run, 300)));
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
}  // namespace
}  // namespace tsugumi::test
