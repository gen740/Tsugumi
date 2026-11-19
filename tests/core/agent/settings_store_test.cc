// The service uses the same explicit input/reply tables with a secret-store
// double. No platform Keychain or credential file is involved. Context-budget
// scenarios below use an external streaming HTTP fixture.
#include <gtest/gtest.h>

#include <algorithm>
#include <thread>

#include "../agent_fixture_server.hh"
#include "agent/runtime.hh"
#include "host/models.hh"
#include "host/settings.hh"
#include "host/store.hh"
#include "tsugumi/util/json_object.hh"

namespace tsugumi::test {
namespace {
namespace H = Tsugumi::Host;
class MemorySecrets final : public host::SecretStore {
 public:
  std::map<std::string, std::string> values;
  bool fail = false;
  auto Get(const std::string& id) -> std::string override { return values[id]; }
  auto Put(const std::string& id, const std::string& secret) -> void override {
    if (fail) throw std::runtime_error(secret);
    values[id] = secret;
  }
  auto Erase(const std::string& id) -> void override { values.erase(id); }
};
TEST(HostSettingsStore,
     ProviderAvailabilityChangesNotifyWithoutProvisioningLoops) {
  TemporaryDirectory directory;
  std::vector<std::string> changed;
  host::Settings service(
      directory.path, [](const auto&, const auto&) {},
      [](const auto&, auto) { return ""; }, [](const auto&) { return true; },
      std::make_unique<MemorySecrets>(), host::Settings::KeyStorage::Host, {},
      [&](const std::string& bytes) {
        const auto* event = H::GetMessage(bytes.data())->payload_as_Event();
        ASSERT_NE(event, nullptr);
        const auto* provider = event->value_as_ProviderChanged();
        ASSERT_NE(provider, nullptr);
        changed.push_back(provider->profile()->str());
      });
  const auto put = [&](bool remove) {
    H::ProviderSetRequestT value;
    value.profile = "desktop";
    value.id = "anthropic";
    value.ephemeral = true;
    value.remove = remove;
    value.provider = std::make_unique<H::ProviderInputT>();
    value.provider->api_key = "FIXTURE_KEY";
    H::RequestT request;
    request.value.Set(std::move(value));
    flatbuffers::FlatBufferBuilder b;
    b.Finish(H::Request::Pack(b, &request));
    return service.Handle(
        *flatbuffers::GetRoot<H::Request>(b.GetBufferPointer()), 1);
  };
  put(false);
  EXPECT_EQ(changed, std::vector<std::string>{"desktop"});
  put(false);
  EXPECT_EQ(changed.size(), 1u);
  put(true);
  EXPECT_EQ(changed, (std::vector<std::string>{"desktop", "desktop"}));
}
TEST(HostSettingsStore, AgentAnimationIsValidatedAndSurvivesReopening) {
  TemporaryDirectory directory;
  const auto make = [&] {
    return std::make_unique<host::Settings>(
        directory.path, [](const auto&, const auto&) {},
        [](const auto&, auto) { return ""; }, [](const auto&) { return true; },
        std::make_unique<MemorySecrets>());
  };
  auto service = make();
  auto call = [&](H::RequestT request) {
    flatbuffers::FlatBufferBuilder b;
    H::MessageT message;
    message.request_id = 1;
    message.payload.Set(std::move(request));
    H::FinishMessageBuffer(b, H::Message::Pack(b, &message));
    return service->Handle(
        *H::GetMessage(b.GetBufferPointer())->payload_as_Request(), 1);
  };
  auto get = [&] {
    H::RequestT request;
    H::SettingsGetRequestT value;
    value.profile = "desktop";
    request.value.Set(std::move(value));
    return call(std::move(request));
  };
  auto animation = [](const std::string& reply) {
    for (const auto* item : *H::GetMessage(reply.data())
                                 ->payload_as_Reply()
                                 ->value_as_Settings()
                                 ->values())
      if (item->key()->str() == "agent.animateWork")
        return item->value_json()->str();
    return std::string{};
  };
  auto update = [&](const std::string& raw) {
    const auto before = get();
    H::SettingsUpdateRequestT value;
    value.profile = "desktop";
    value.revision = H::GetMessage(before.data())
                         ->payload_as_Reply()
                         ->value_as_Settings()
                         ->revision();
    auto item = std::make_unique<H::SettingT>();
    item->key = "agent.animateWork";
    item->value_json = raw;
    value.changes.push_back(std::move(item));
    H::RequestT request;
    request.value.Set(std::move(value));
    return call(std::move(request));
  };
  EXPECT_EQ(animation(get()), "true");
  const auto invalid = update(Quote("false"));
  EXPECT_NE(
      H::GetMessage(invalid.data())->payload_as_Reply()->value_as_Failure(),
      nullptr);
  EXPECT_EQ(animation(get()), "true");
  EXPECT_EQ(animation(update("false")), "false");
  service.reset();
  service = make();
  EXPECT_EQ(animation(get()), "false");
}
TEST(HostSettingsStore,
     ProviderInputsStayInTheSecretStoreAndFailuresAreSanitized) {
  TemporaryDirectory directory;
  auto memory = std::make_unique<MemorySecrets>();
  auto* observed = memory.get();
  host::Settings service(
      directory.path, [](const auto&, const auto&) {},
      [](const auto&, auto) { return ""; }, [](const auto&) { return true; },
      std::move(memory), host::Settings::KeyStorage::Host);
  const auto put = [&](const std::string& secret) {
    flatbuffers::FlatBufferBuilder b;
    Tsugumi::Host::ProviderInputT value;
    value.api_key = secret;
    const auto p = H::CreateProviderSetRequest(
        b, b.CreateString("desktop"), b.CreateString("anthropic"),
        Tsugumi::Host::ProviderInput::Pack(b, &value));
    const auto req =
        H::CreateRequest(b, H::RequestPayload::ProviderSetRequest, p.Union());
    H::FinishMessageBuffer(
        b, H::CreateMessage(b, 1, H::Payload::Request, req.Union()));
    return service.Handle(
        *H::GetMessage(b.GetBufferPointer())->payload_as_Request(), 1);
  };
  const auto reply = put("STORE_SECRET_SENTINEL");
  EXPECT_TRUE(H::GetMessage(reply.data())
                  ->payload_as_Reply()
                  ->value_as_ProviderStatus()
                  ->configured());
  EXPECT_EQ(reply.find("STORE_SECRET_SENTINEL"), reply.npos);
  EXPECT_EQ(observed->values.at("desktop:anthropic"), "STORE_SECRET_SENTINEL");
  const auto config = service.Resolve({});
  EXPECT_EQ(config.credential, "STORE_SECRET_SENTINEL");
  EXPECT_EQ(service.Redact("echo STORE_SECRET_SENTINEL"), "echo [redacted]");
  const auto ephemeral = [&](const std::string& profile,
                             const std::string& secret) {
    flatbuffers::FlatBufferBuilder b;
    Tsugumi::Host::ProviderInputT value;
    value.api_key = secret;
    const auto p = H::CreateProviderSetRequest(
        b, b.CreateString(profile), b.CreateString("anthropic"),
        Tsugumi::Host::ProviderInput::Pack(b, &value), false, true);
    const auto req =
        H::CreateRequest(b, H::RequestPayload::ProviderSetRequest, p.Union());
    H::FinishMessageBuffer(
        b, H::CreateMessage(b, 2, H::Payload::Request, req.Union()));
    return service.Handle(
        *H::GetMessage(b.GetBufferPointer())->payload_as_Request(), 2);
  };
  const auto env = ephemeral("web:env:first", "FIRST_ENV_SENTINEL");
  ephemeral("web:env:second", "SECOND_ENV_SENTINEL");
  EXPECT_EQ(env.find("FIRST_ENV_SENTINEL"), env.npos);
  EXPECT_FALSE(observed->values.contains("web:env:first:anthropic"));
  EXPECT_EQ(service
                .Resolve({{"profile", Quote("desktop")},
                          {"key_profile", Quote("web:env:first")}})
                .credential,
            "FIRST_ENV_SENTINEL");
  EXPECT_EQ(service
                .Resolve({{"profile", Quote("desktop")},
                          {"key_profile", Quote("web:env:second")}})
                .credential,
            "SECOND_ENV_SENTINEL");
  EXPECT_EQ(service.Resolve({}).credential, "STORE_SECRET_SENTINEL");
  EXPECT_EQ(service
                .Resolve({{"profile", Quote("desktop")},
                          {"key_profile", Quote("missing")}})
                .credential,
            "STORE_SECRET_SENTINEL");
  observed->fail = true;
  const auto failed = put("SECOND_SECRET_SENTINEL");
  EXPECT_NE(
      H::GetMessage(failed.data())->payload_as_Reply()->value_as_Failure(),
      nullptr);
  EXPECT_EQ(failed.find("SECOND_SECRET_SENTINEL"), failed.npos);
}
TEST(HostSettingsStore,
     KeychainProvisioningNeverWritesTheKeyToDiskOrFallsBack) {
  TemporaryDirectory directory;
  const std::string key = "KEYCHAIN_ONLY_SENTINEL";
  const auto make = [&] {
    return std::make_unique<host::Settings>(
        directory.path, [](const auto&, const auto&) {},
        [](const auto&, auto) { return ""; }, [](const auto&) { return true; },
        nullptr, host::Settings::KeyStorage::Keychain);
  };
  auto service = make();
  flatbuffers::FlatBufferBuilder b;
  Tsugumi::Host::ProviderInputT input;
  input.api_key = key;
  const auto value = H::CreateProviderSetRequest(
      b, b.CreateString("macos:keychain:fixture"), b.CreateString("anthropic"),
      Tsugumi::Host::ProviderInput::Pack(b, &input), false, true);
  const auto request =
      H::CreateRequest(b, H::RequestPayload::ProviderSetRequest, value.Union());
  H::FinishMessageBuffer(
      b, H::CreateMessage(b, 1, H::Payload::Request, request.Union()));
  const auto reply = service->Handle(
      *H::GetMessage(b.GetBufferPointer())->payload_as_Request(), 1);
  EXPECT_EQ(reply.find(key), reply.npos);
  ASSERT_TRUE(H::GetMessage(reply.data())
                  ->payload_as_Reply()
                  ->value_as_ProviderStatus()
                  ->configured());
  const ::tsugumi::json::Members selection{
      {"profile", Quote("desktop")},
      {"key_profile", Quote("macos:keychain:fixture")}};
  EXPECT_EQ(service->Resolve(selection).credential, key);
  for (const auto& file : std::filesystem::directory_iterator(directory.path))
    if (file.is_regular_file())
      EXPECT_EQ(Read(file.path()).find(key), std::string::npos);
  service.reset();
  service = make();
  EXPECT_THROW(service->Resolve(selection), std::runtime_error);
  for (const auto& file : std::filesystem::directory_iterator(directory.path))
    if (file.is_regular_file())
      EXPECT_EQ(Read(file.path()).find(key), std::string::npos);
}
}  // namespace
}  // namespace tsugumi::test

namespace tsugumi::test {
TEST(HostSettingsStore, VMSizeFollowsTheDefaultsUntilSetWithinLimits) {
  TemporaryDirectory directory;
  host::Settings service(
      directory.path, [](const auto&, const auto&) {},
      [](const auto&, auto) { return ""; }, [](const auto&) { return true; },
      std::make_unique<MemorySecrets>());
  const auto update = [&](const std::string& profile, const std::string& key,
                          const std::string& raw) {
    H::SettingsGetRequestT get;
    get.profile = profile;
    H::RequestT read;
    read.value.Set(std::move(get));
    flatbuffers::FlatBufferBuilder rb;
    rb.Finish(H::Request::Pack(rb, &read));
    const auto before = service.Handle(
        *flatbuffers::GetRoot<H::Request>(rb.GetBufferPointer()), 1);
    H::SettingsUpdateRequestT value;
    value.profile = profile;
    value.revision = H::GetMessage(before.data())
                         ->payload_as_Reply()
                         ->value_as_Settings()
                         ->revision();
    auto item = std::make_unique<H::SettingT>();
    item->key = key;
    item->value_json = raw;
    value.changes.push_back(std::move(item));
    H::RequestT request;
    request.value.Set(std::move(value));
    flatbuffers::FlatBufferBuilder b;
    b.Finish(H::Request::Pack(b, &request));
    const auto reply = service.Handle(
        *flatbuffers::GetRoot<H::Request>(b.GetBufferPointer()), 1);
    return H::GetMessage(reply.data())
               ->payload_as_Reply()
               ->value_as_Failure() == nullptr;
  };
  const auto defaults = host::VMDefaultResources();
  const auto limits = host::VMResourceLimits();
  EXPECT_GE(defaults.cpus, 2u);
  EXPECT_LE(defaults.cpus, std::min(8u, limits.cpus));
  EXPECT_GE(defaults.memory_gib, 4u);
  EXPECT_LE(defaults.memory_gib, 16u);
  EXPECT_EQ(defaults.store_gib, 64u);
  EXPECT_EQ(service.VMResources().cpus, defaults.cpus);
  EXPECT_TRUE(update("desktop", "vm.cpus", "1"));
  EXPECT_TRUE(update("desktop", "vm.storeGiB", "128"));
  EXPECT_EQ(service.VMResources().cpus, 1u);
  EXPECT_EQ(service.VMResources().store_gib, 128u);
  EXPECT_EQ(service.VMResources().memory_gib, defaults.memory_gib);
  EXPECT_FALSE(update("desktop", "vm.cpus", std::to_string(limits.cpus + 1)));
  EXPECT_FALSE(update("desktop", "vm.memoryGiB", "1"));
  EXPECT_FALSE(update("desktop", "vm.storeGiB", "8"));
  EXPECT_FALSE(update("desktop", "vm.storeGiB", Quote("64")));
  EXPECT_FALSE(update("web:/somewhere", "vm.cpus", "2"));
  EXPECT_FALSE(update("desktop", "vm.other", "2"));
  EXPECT_TRUE(update("desktop", "vm.cpus", "0"));
  EXPECT_EQ(service.VMResources().cpus, defaults.cpus);
}
TEST(HostSettingsStore, VersionNotesUseAnExplicitModeAndRejectOtherValues) {
  TemporaryDirectory directory;
  auto memory = std::make_unique<MemorySecrets>();
  auto* secrets = memory.get();
  secrets->values["desktop:anthropic"] = "ANTHROPIC_NOTE_KEY";
  secrets->values["desktop:openai"] = "OPENAI_NOTE_KEY";
  host::Settings service(
      directory.path, [](const auto&, const auto&) {},
      [](const auto&, auto) { return ""; }, [](const auto&) { return true; },
      std::move(memory), host::Settings::KeyStorage::Host);
  EXPECT_EQ(service.VersionNoteMode("desktop"), "mechanical");
  const auto update = [&](const std::string& mode, uint64_t revision,
                          const std::string& key = "versions.noteMode") {
    flatbuffers::FlatBufferBuilder b;
    const auto change =
        H::CreateSetting(b, b.CreateString(key), b.CreateString(mode));
    const auto p =
        H::CreateSettingsUpdateRequest(b, b.CreateString("desktop"), revision,
                                       b.CreateVector(std::vector{change}));
    const auto request = H::CreateRequest(
        b, H::RequestPayload::SettingsUpdateRequest, p.Union());
    H::FinishMessageBuffer(
        b, H::CreateMessage(b, 1, H::Payload::Request, request.Union()));
    return service.Handle(
        *H::GetMessage(b.GetBufferPointer())->payload_as_Request(), 1);
  };
  for (const auto& invalid : {"true", "\"unknown\""}) {
    const auto failure = update(invalid, 0);
    ASSERT_TRUE(
        H::GetMessage(failure.data())->payload_as_Reply()->value_as_Failure());
  }
  auto reply = update("\"agent\"", 0);
  ASSERT_TRUE(
      H::GetMessage(reply.data())->payload_as_Reply()->value_as_Settings());
  EXPECT_EQ(service.VersionNoteMode("desktop"), "agent");
  EXPECT_EQ(service.VersionNoteMode("web:other"), "mechanical");
  reply = update("\"mechanical\"", 1);
  ASSERT_TRUE(
      H::GetMessage(reply.data())->payload_as_Reply()->value_as_Settings());
  EXPECT_EQ(service.VersionNoteMode("desktop"), "mechanical");
  EXPECT_EQ(service.VersionNoteProvider("desktop", "", "").credential,
            "ANTHROPIC_NOTE_KEY");
  EXPECT_EQ(service.VersionNoteProvider("desktop", "", "").model,
            "claude-haiku-5-5");
  EXPECT_EQ(service.VersionNoteProvider("desktop", "missing-platform-keys", "")
                .credential,
            "ANTHROPIC_NOTE_KEY");
  reply = update("\"openai\"", 2, "agent.provider");
  ASSERT_TRUE(
      H::GetMessage(reply.data())->payload_as_Reply()->value_as_Settings());
  EXPECT_EQ(service.VersionNoteProvider("desktop", "missing-anthropic-keys", "")
                .credential,
            "OPENAI_NOTE_KEY");
  reply = update("\"custom-small-model\"", 3, "agent.thoughtModel.openai");
  ASSERT_TRUE(
      H::GetMessage(reply.data())->payload_as_Reply()->value_as_Settings());
  EXPECT_EQ(service.VersionNoteProvider("desktop", "", "").model,
            "custom-small-model");
}
}  // namespace tsugumi::test

namespace tsugumi::test {
namespace {
class HostPolicyTest : public ::testing::Test {
 protected:
  TemporaryDirectory directory;
  std::unique_ptr<host::Settings> service;
  MemorySecrets* secrets = nullptr;
  std::vector<std::string> models;
  uint64_t revision = 0;
  auto Open(host::Settings::KeyStorage storage) -> void {
    auto store = std::make_unique<MemorySecrets>();
    secrets = store.get();
    service = std::make_unique<host::Settings>(
        directory.path, [](const auto&, const auto&) {},
        [](const auto&, auto) { return ""; }, [](const auto&) { return true; },
        std::move(store), storage, [this] { return models; });
  }
  template <typename T>
  auto Call(T value) -> std::unique_ptr<H::ReplyT> {
    H::RequestT request;
    request.value.Set(std::move(value));
    flatbuffers::FlatBufferBuilder builder;
    const auto packed = H::Request::Pack(builder, &request);
    builder.Finish(packed);
    const auto bytes = service->Handle(
        *flatbuffers::GetRoot<H::Request>(builder.GetBufferPointer()), 1);
    return std::unique_ptr<H::ReplyT>(
        H::GetMessage(bytes.data())->payload_as_Reply()->UnPack());
  }
  auto Set(const std::string& key, const std::string& value) -> void {
    H::SettingsUpdateRequestT request;
    request.profile = "desktop";
    request.revision = revision;
    auto change = std::make_unique<H::SettingT>();
    change->key = key;
    change->value_json = Quote(value);
    request.changes.push_back(std::move(change));
    const auto reply = Call(std::move(request));
    ASSERT_NE(reply->value.AsSettings(), nullptr);
    revision = reply->value.AsSettings()->revision;
  }
  auto Provision(const std::string& source, const std::string& secret = "")
      -> void {
    H::ProviderSetRequestT request;
    request.profile = "client";
    request.id = "anthropic";
    request.source = source;
    request.ephemeral = true;
    request.remove = secret.empty();
    if (!secret.empty()) {
      request.provider = std::make_unique<Tsugumi::Host::ProviderInputT>();
      request.provider->api_key = secret;
    }
    ASSERT_NE(Call(std::move(request))->value.AsProviderStatus(), nullptr);
  }
  auto Status() -> H::ProviderStatusT {
    H::ProviderStatusRequestT request;
    request.profile = "desktop";
    request.id = "anthropic";
    request.key_profile = "client";
    return *Call(std::move(request))->value.AsProviderStatus();
  }
  auto Resolve() -> agent::ProviderConfig {
    return service->Resolve(
        {{"profile", Quote("desktop")}, {"key_profile", Quote("client")}});
  }
};
TEST_F(HostPolicyTest, MacRejectsDiskKeysAndStatusAgreesWithResolution) {
  Open(host::Settings::KeyStorage::Keychain);
  secrets->values["desktop:anthropic"] = "DISK_MUST_NOT_BE_USED";
  EXPECT_FALSE(Status().configured);
  EXPECT_EQ(Status().storage, "keychain");
  EXPECT_THROW(Resolve(), std::runtime_error);
  H::ProviderSetRequestT request;
  request.profile = "desktop";
  request.id = "anthropic";
  request.provider = std::make_unique<Tsugumi::Host::ProviderInputT>();
  request.provider->api_key = "REJECTED_DISK_KEY";
  const auto reply = Call(std::move(request));
  ASSERT_NE(reply->value.AsFailure(), nullptr);
  EXPECT_EQ(secrets->values["desktop:anthropic"], "DISK_MUST_NOT_BE_USED");
  Provision("keychain", "KEYCHAIN_KEY");
  EXPECT_TRUE(Status().configured);
  EXPECT_EQ(Status().source, "keychain");
  EXPECT_EQ(Resolve().credential, "KEYCHAIN_KEY");
  Provision("env", "ENV_KEY");
  EXPECT_EQ(Status().source, "env");
  EXPECT_EQ(Resolve().credential, "ENV_KEY");
  Provision("env");
  EXPECT_EQ(Resolve().credential, "KEYCHAIN_KEY");
  Provision("keychain");
  EXPECT_FALSE(Status().configured);
  try {
    Resolve();
    FAIL();
  } catch (const std::runtime_error& error) {
    EXPECT_EQ(error.what(), Status().message);
  }
}
TEST_F(HostPolicyTest, LinuxEnvironmentOverridesStoredKeyWithoutReplacingIt) {
  Open(host::Settings::KeyStorage::Host);
  secrets->values["desktop:anthropic"] = "STORED_KEY";
  EXPECT_TRUE(Status().configured);
  EXPECT_EQ(Status().storage, "hostd");
  EXPECT_EQ(Resolve().credential, "STORED_KEY");
  Provision("env", "ENV_KEY");
  EXPECT_EQ(Status().source, "env");
  EXPECT_EQ(Resolve().credential, "ENV_KEY");
  EXPECT_EQ(secrets->values["desktop:anthropic"], "STORED_KEY");
  Provision("env");
  EXPECT_EQ(Status().source, "hostd");
  EXPECT_EQ(Resolve().credential, "STORED_KEY");
}
TEST_F(HostPolicyTest, GuideAndSummaryUseSettingsAndHostCatalog) {
  Open(host::Settings::KeyStorage::Host);
  EXPECT_EQ(service->ModelSelection("desktop", "guide").model,
            "claude-haiku-5-5");
  Set("agent.thoughtModel", "custom-thought");
  EXPECT_EQ(service->ModelSelection("desktop", "summary").model,
            "custom-thought");
  EXPECT_EQ(service->ModelSelection("desktop", "guide").model,
            "claude-haiku-5-5");
  Provision("env", "ENV_KEY");
  auto config = service->Resolve({{"profile", Quote("desktop")},
                                  {"purpose", Quote("summary")},
                                  {"model", Quote("ignored-client-model")},
                                  {"key_profile", Quote("client")}});
  EXPECT_EQ(config.model, "custom-thought");
  Set("agent.provider", "openai");
  Set("agent.thoughtModel.openai", "small-openai");
  EXPECT_EQ(service->ModelSelection("desktop", "summary").model,
            "small-openai");
  EXPECT_NE(service->ModelSelection("desktop", "guide").model, "small-openai");
  Set("agent.provider", "chatgpt");
  const auto fallback = service->ModelSelection("desktop", "guide").model;
  EXPECT_FALSE(fallback.empty());
  models = {"gpt-6-luna", "gpt-5.6-luna", "gpt-6.10-luna"};
  EXPECT_EQ(service->ModelSelection("desktop", "guide").model, "gpt-6.10-luna");
  models.push_back("gpt-6.1-luna");
  EXPECT_EQ(service->ModelSelection("desktop", "summary").model,
            "gpt-6.1-luna");
  Set("agent.speechModel.chatgpt", "speech-override");
  H::ModelSelectionRequestT request;
  request.profile = "desktop";
  request.purpose = "guide";
  const auto reply = Call(std::move(request));
  ASSERT_NE(reply->value.AsModelSelection(), nullptr);
  EXPECT_EQ(reply->value.AsModelSelection()->provider->model,
            "speech-override");
  EXPECT_EQ(service->ModelSelection("desktop", "summary").model,
            "speech-override");
  EXPECT_THROW(service->ModelSelection("desktop", "unknown"),
               std::runtime_error);
  Set("agent.provider", "fixture");
  EXPECT_EQ(service->ModelSelection("desktop", "summary").model, "fixture");
}

// These requests use the real runtime and external streaming HTTP provider.
class ContextBudget : public testing::Test {
 protected:
  ModelServer model;
  agent::ProviderConfig config;
  agent::ContextUsage context;
  std::vector<agent::Message> messages;
  std::vector<std::string> events;
  agent::RuntimeHooks hooks;
  ContextBudget() {
    config.base_url = model.URL();
    config.auth = agent::ProviderAuth::None;
    config.model = "fixture";
    config.context_limit_tokens = 200000;
    hooks.authorize_context = [](const auto&) {};
    hooks.checkpoint = [](const auto&) {};
    hooks.event = [this](const auto& event) { events.push_back(event.kind); };
  }
  auto Add(std::string text, std::string role = "user") -> void {
    agent::ContentBlock block;
    block.text = std::move(text);
    agent::Message message;
    message.role = std::move(role);
    message.content.push_back(std::move(block));
    messages.push_back(std::move(message));
  }
  auto Run(std::string_view system = "system",
           const std::vector<agent::ToolSpec>& tools = {}) -> void {
    const auto result = agent::Runtime().Run(config, system, messages, tools,
                                             {}, hooks, &context);
    ASSERT_EQ(result.outcome, agent::RunOutcome::Finished) << result.error;
  }
  auto Compacted() -> bool {
    return std::find(events.begin(), events.end(), "compacted") != events.end();
  }
};

TEST_F(ContextBudget,
       InternalToolFailureEndsTheTurnWithoutAskingTheModelToRetry) {
  Add("Implement the Draft");
  model.Queue(AnthropicTool("edit", "project_edit", "{}"));
  int calls = 0;
  hooks.execute_tool = [&](const auto&, auto) {
    ++calls;
    return agent::ToolResult{
        R"({"reply":"Failure","value":{"kind":"internal_error","diagnostic":"HOST_IDENTITY_SENTINEL"}})",
        true,
        {},
        {}};
  };
  const auto result = agent::Runtime().Run(config, "system", messages,
                                           {{"project_edit", "edit", "{}"}}, {},
                                           hooks, &context);
  EXPECT_EQ(result.outcome, agent::RunOutcome::Failed);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(model.Requests().size(), 1u);
  EXPECT_NE(result.error.find("HOST_IDENTITY_SENTINEL"), std::string::npos);
  EXPECT_FALSE(messages.back().content.size());
}

TEST_F(ContextBudget, RepairableRefusalsAndNonJsonErrorsRemainToolResults) {
  for (
      const auto* text :
      {R"({"reply":"Failure","value":{"kind":"validation_failed","diagnostic":"write to.port"}})",
       "{incomplete tool output"}) {
    messages.clear();
    Add("Implement the Draft");
    model.Queue(AnthropicTool("edit", "project_edit", "{}"));
    model.Queue(AnthropicText("done"));
    hooks.execute_tool = [text](const auto&, auto) {
      return agent::ToolResult{text, true, {}, {}};
    };
    const auto result = agent::Runtime().Run(config, "system", messages,
                                             {{"project_edit", "edit", "{}"}},
                                             {}, hooks, &context);
    EXPECT_EQ(result.outcome, agent::RunOutcome::Finished) << result.error;
  }
  EXPECT_EQ(model.Requests().size(), 4u);
}

TEST_F(ContextBudget, ConsecutiveRequestsHaveByteIdenticalCachedPrefixes) {
  for (const auto api : {agent::ProviderAPI::AnthropicMessages,
                         agent::ProviderAPI::OpenAIResponses}) {
    config.api = api;
    messages.clear();
    context = {};
    Add("Keep the original request and its stable identifiers.");
    for (int step = 1; step <= 2; ++step) {
      const auto id = "stable-call-" + std::to_string(step);
      if (api == agent::ProviderAPI::AnthropicMessages) {
        model.Queue(AnthropicTool(id, "read", "{}"));
      } else {
        const auto item = "{\"type\":\"function_call\",\"id\":\"fc_" + id +
                          "\",\"call_id\":\"" + id +
                          "\",\"name\":\"read\",\"arguments\":\"{}\"}";
        model.Queue(
            "event: response.output_item.added\ndata: "
            "{\"type\":\"response.output_item.added\",\"output_index\":0,"
            "\"item\":" +
            item +
            "}\n\nevent: response.completed\ndata: "
            "{\"type\":\"response.completed\",\"response\":{\"output\":[" +
            item + "]}}\n\n");
      }
    }
    model.Queue(api == agent::ProviderAPI::AnthropicMessages
                    ? AnthropicText("done")
                    : ResponsesText("done"));
    hooks.execute_tool = [](const auto&, auto) {
      return agent::ToolResult{"stable result", false, {}, {}};
    };
    const auto start = model.Requests().size();
    Run("stable system", {{"z_read", "last", "{}"}, {"read", "read", "{}"}});
    const auto requests = model.Requests();
    ASSERT_EQ(requests.size(), start + 3);
    for (size_t step = start; step < start + 2; ++step) {
      const auto first = ::tsugumi::json::Object(requests[step]);
      const auto second = ::tsugumi::json::Object(requests[step + 1]);
      const auto field =
          api == agent::ProviderAPI::AnthropicMessages ? "messages" : "input";
      const auto prefix = ::tsugumi::json::Get(first, field);
      const auto marker = ::tsugumi::json::Quote(field) + ":" + prefix;
      const auto at = requests[step].find(marker);
      ASSERT_NE(at, std::string::npos);
      const auto end = at + marker.size() - 1;
      EXPECT_EQ(requests[step + 1].substr(0, end),
                requests[step].substr(0, end));
      for (const auto* key : {"tools", "system", "instructions", "model"})
        EXPECT_EQ(::tsugumi::json::Get(first, key),
                  ::tsugumi::json::Get(second, key));
      if (api == agent::ProviderAPI::AnthropicMessages)
        EXPECT_EQ(
            ::tsugumi::json::Text(
                ::tsugumi::json::Object(first.at("cache_control")), "type"),
            "ephemeral");
    }
  }
}

TEST_F(ContextBudget, ProviderCatalogPreservesReportedWindowsAndMissingValues) {
  model.QueueHTTP(
      R"({"data":[{"id":"reported","max_input_tokens":1000000},{"id":"absent"},{"id":"null","max_input_tokens":null}]})");
  const auto catalog = agent::ListModels(config, {});
  ASSERT_EQ(catalog.size(), 3u);
  EXPECT_EQ(catalog[0].context_window_tokens, 1000000u);
  EXPECT_EQ(catalog[1].context_window_tokens, 0u);
  EXPECT_EQ(catalog[2].context_window_tokens, 0u);
}

TEST_F(ContextBudget, ChatGPTCatalogUsesTheAccountsActiveWindow) {
  config.auth = agent::ProviderAuth::ChatGPT;
  config.credential = "TEST_TOKEN";
  model.QueueHTTP(
      R"({"models":[{"slug":"gpt-6.1-sol","display_name":"Sol","visibility":"list","context_window":500000,"max_context_window":1050000},{"slug":"other","visibility":"list","context_window":null,"max_context_window":272000},{"slug":"hidden","visibility":"hide","context_window":1000000}]})");
  const auto catalog = agent::ListModels(config, {});
  ASSERT_EQ(catalog.size(), 2u);
  EXPECT_EQ(catalog[0].id, "gpt-6.1-sol");
  EXPECT_EQ(catalog[0].context_window_tokens, 500000u);
  EXPECT_EQ(catalog[1].context_window_tokens, 272000u);
}

TEST_F(ContextBudget, EscapedBytesDoNotTriggerCompactionAndUsageSurvivesTurns) {
  // Quoted tool/code-like text exceeds the old 180KB storage threshold.
  std::string quoted;
  for (int i = 0; i < 30000; ++i) quoted += "\"\\\n";
  for (int i = 0; i < 6; ++i) Add(quoted.substr(0, 15000));
  ASSERT_GT(agent::SerializeMessages(messages).size(), 180000u);
  model.Queue(AnthropicText(std::string(110000, 'a'), 1000));
  Run();
  ASSERT_FALSE(Compacted());
  // A full-text estimate now exceeds the budget. The measured request plus
  // only its appended assistant/user messages still fits comfortably.
  Add("continue");
  model.Queue(AnthropicText("done", 5000));
  Run();
  EXPECT_FALSE(Compacted());
  EXPECT_EQ(model.Requests().size(), 2u);
  EXPECT_EQ(context.input_tokens, 5000u);
}

TEST_F(ContextBudget,
       SummaryKeepsCompleteRecentJapaneseResultsWithoutStreamingIt) {
  config.context_limit_tokens = 2000;
  Add("read");
  auto stream = AnthropicTool("call-1", "read", "{}");
  const auto at = stream.find("\"input_tokens\":12");
  ASSERT_NE(at, std::string::npos);
  stream.replace(at, std::string("\"input_tokens\":12").size(),
                 "\"input_tokens\":1500");
  model.Queue(stream);
  model.Queue(AnthropicText("INTERNAL_HANDOFF"));
  model.Queue(AnthropicText("done"));
  std::string text;
  for (int i = 0; i < 250; ++i) text += "実験";
  hooks.execute_tool = [&](const auto&, auto) {
    return agent::ToolResult{text, false, {}, {}};
  };
  std::string visible;
  hooks.event = [&](const auto& event) {
    events.push_back(event.kind);
    if (event.kind == "text") visible += event.text;
  };
  Run("system", {{"read", "read", "{}"}});
  ASSERT_TRUE(Compacted());
  const auto requests = model.Requests();
  ASSERT_EQ(requests.size(), 3u);
  EXPECT_EQ(visible, "done");
  EXPECT_EQ(JsonString(requests[2], "/messages/2/content/0/content"), text);
  EXPECT_EQ(messages[2].content[0].text, text);
  const auto summary = ::tsugumi::json::Object(requests[1]);
  const auto normal = ::tsugumi::json::Object(requests[0]);
  EXPECT_EQ(summary.at("system"), normal.at("system"));
  EXPECT_EQ(summary.at("tools"), normal.at("tools"));
}

TEST_F(ContextBudget, ReportedInputPlusNewMessagesCompactsBeforeRefusal) {
  for (int i = 0; i < 6; ++i) Add("earlier decision");
  model.Queue(AnthropicText(std::string(2000, 'x'), 199000));
  Run();
  ASSERT_FALSE(Compacted());
  Add("continue");
  model.Queue(AnthropicText("handoff", 3000));
  model.Queue(AnthropicText("done", 4000));
  Run();
  ASSERT_TRUE(Compacted());
  const auto requests = model.Requests();
  ASSERT_EQ(requests.size(), 3u);
  EXPECT_EQ(context.input_tokens, 4000u);  // Summary usage is not the baseline.
}

TEST_F(ContextBudget,
       JapaneseWithoutUsageIsConservativeAndCountsSystemAndTools) {
  config.context_limit_tokens = 10000;
  std::string japanese;
  for (int i = 0; i < 300; ++i) japanese += "実験";
  EXPECT_EQ(agent::EstimateTextTokens(japanese), japanese.size());
  for (int i = 0; i < 6; ++i) Add(japanese);
  model.Queue(AnthropicText("handoff"));
  model.Queue(AnthropicText("done", 0));
  Run();
  EXPECT_TRUE(Compacted());
  ASSERT_EQ(model.Requests().size(), 2u);
  // A restored transcript has no valid request baseline. System/tools also
  // occupy the window, even though SerializeMessages omits both of them.
  context = {};
  events.clear();
  for (int i = 0; i < 3; ++i) Add(japanese);
  model.Queue(AnthropicText("handoff"));
  model.Queue(AnthropicText("done"));
  Run(std::string(1500, 's'), {{"tool", std::string(1500, 't'), "{}"}});
  EXPECT_TRUE(Compacted());
}

TEST_F(ContextBudget,
       MissingUsageFitsAndModelChangesInvalidateMeasuredBaseline) {
  config.context_limit_tokens = 10000;
  for (int i = 0; i < 6; ++i) Add(std::string(1000, 'x'));
  model.Queue(AnthropicText("first", 0));
  Run();
  EXPECT_FALSE(Compacted());
  EXPECT_EQ(context.input_tokens, 0u);
  Add("continue");
  model.Queue(AnthropicText("second", 1));
  Run();
  config.model = "another-model";
  Add("continue");
  model.Queue(AnthropicText("handoff"));
  model.Queue(AnthropicText("done"));
  Run(std::string(6000, 's'));
  EXPECT_TRUE(Compacted());
}

TEST_F(ContextBudget,
       OldResultsMoveInBatchesAndTheLatestResultAndRecordSurvive) {
  config.context_limit_tokens = 100000;
  Add("inspect all records");
  for (int i = 1; i <= 15; ++i) {
    auto stream = AnthropicTool("inspect-" + std::to_string(i), "inspect",
                                "{\"card\":\"a\"}");
    const std::string marker = "\"input_tokens\":12";
    stream.replace(stream.find(marker), marker.size(), "\"input_tokens\":0");
    model.Queue(std::move(stream));
  }
  model.Queue(AnthropicText("done", 0));
  hooks.execute_tool = [](const auto& call, auto) {
    return agent::ToolResult{
        call.id + " HEAD " + std::string(40000, 'x') + " TAIL " + call.id,
        false,
        {},
        {}};
  };
  Run("system", {{"inspect", "Inspect", "{}"}});
  EXPECT_FALSE(Compacted());
  EXPECT_GT(context.omitted_before, 0u);
  const auto requests = model.Requests();
  ASSERT_EQ(requests.size(), 16u);
  size_t rewrites = 0;
  for (size_t i = 1; i < requests.size(); ++i) {
    const auto latest =
        JsonString(requests[i],
                   "/messages/" + std::to_string(2 * i) + "/content/0/content");
    EXPECT_TRUE(latest.ends_with("TAIL inspect-" + std::to_string(i)));
    const auto before = ::tsugumi::json::Get(
        ::tsugumi::json::Object(requests[i - 1]), "messages");
    const auto after =
        ::tsugumi::json::Get(::tsugumi::json::Object(requests[i]), "messages");
    if (!after.starts_with(before.substr(0, before.size() - 1))) ++rewrites;
  }
  EXPECT_GT(rewrites, 0u);
  EXPECT_LT(rewrites, 7u);  // Rewriting each step would defeat prefix caching.
  const auto omitted =
      JsonString(requests.back(), "/messages/2/content/0/content");
  EXPECT_NE(omitted.find("inspect {\"card\":\"a\"}"), std::string::npos);
  EXPECT_NE(omitted.find("tool_result_read"), std::string::npos);
  for (const auto& message : messages)
    for (const auto& block : message.content)
      if (block.type == "tool_result") EXPECT_GT(block.text.size(), 40000u);
  const auto cut = context.omitted_before;
  Add("continue");
  model.Queue(AnthropicText("continued", 0));
  Run("system", {{"inspect", "Inspect", "{}"}});
  EXPECT_EQ(context.omitted_before, cut);
}

TEST_F(ContextBudget, CompactionKeepsRecentTokensAndReplaysSignedThinking) {
  config.context_limit_tokens = 100000;
  config.effort = "max";
  for (int i = 0; i < 6; ++i) Add(std::string(10000, 'x'));
  for (int i = 0; i < 30; ++i) Add(std::string(180, 'r'));
  messages[1].role = "assistant";
  messages[1].provider_content =
      R"([{"type":"thinking","thinking":"retained reasoning","signature":"signed-block"},{"type":"text","text":"original answer"}])";
  const auto original = agent::SerializeMessages(messages);
  std::string visible;
  hooks.event = [&](const auto& event) {
    events.push_back(event.kind);
    if (event.kind == "text" || event.kind == "toolInput")
      visible += event.text;
  };
  model.Queue(AnthropicText("INTERNAL_HANDOFF"));
  agent::Runtime().Compact(config, "unchanged system", messages,
                           {{"inspect", "Inspect", "{}"}}, {}, hooks, &context);
  EXPECT_TRUE(Compacted());
  EXPECT_TRUE(visible.empty());
  EXPECT_TRUE(messages.front().context_summary);
  EXPECT_EQ(messages.back().content[0].text, std::string(180, 'r'));
  const auto requests = model.Requests();
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_NE(requests[0].find("signed-block"), std::string::npos);
  EXPECT_NE(requests[0].find("unchanged system"), std::string::npos);
  EXPECT_NE(requests[0].find("\"name\":\"inspect\""), std::string::npos);
  EXPECT_EQ(JsonString(requests[0], "/output_config/effort"), "max");
  // A failed continuation leaves the durable transcript untouched.
  const auto saved = agent::SerializeMessages(messages);
  model.QueueHTTP(R"({"error":{"message":"fixture refusal"}})", 400);
  EXPECT_THROW(
      agent::Runtime().Compact(config, "unchanged system", messages,
                               {{"inspect", "Inspect", "{}"}}, {}, hooks),
      agent::ProviderError);
  EXPECT_EQ(agent::SerializeMessages(messages), saved);
  EXPECT_NE(original.find("signed-block"), std::string::npos);
}

TEST_F(ContextBudget, OverLimitConversationSummarizesOnlyTheFittingPast) {
  config.context_limit_tokens = 128000;
  for (int i = 0; i < 3; ++i) Add(std::string(20000, 'p'));
  agent::ContentBlock call;
  call.type = "tool_use";
  call.id = "past-call";
  call.name = "inspect";
  messages.push_back(
      {"assistant",
       {call},
       {},
       R"([{"type":"thinking","thinking":"past reasoning","signature":"past-signature"},{"type":"tool_use","id":"past-call","name":"inspect","input":{}}])",
       agent::ProviderAPI::AnthropicMessages});
  call.type = "tool_result";
  call.text = "past result";
  messages.push_back(
      {"user", {call}, {}, {}, agent::ProviderAPI::AnthropicMessages});
  const auto recent = "LATEST_REQUEST " + std::string(80000, 'r');
  Add(recent);
  std::jthread provider([&](std::stop_token stop) {
    size_t next = 1;
    while (!stop.stop_requested()) {
      if (!model.WaitForRequests(next, stop)) break;
      const auto request = model.Requests()[next++ - 1];
      // A real provider refuses the old whole-conversation summary too.
      if (request.size() >= config.context_limit_tokens)
        model.QueueHTTP(R"({"error":{"message":"prompt is too long"}})", 400);
      else if (next == 2)
        model.Queue(AnthropicText("Want: continue. Done: past inspection."));
      else
        model.Queue(AnthropicText("done"));
    }
  });
  const auto result =
      agent::Runtime().Run(config, "system", messages,
                           {{"inspect", "Inspect", "{}"}}, {}, hooks, &context);
  provider.request_stop();
  provider.join();
  ASSERT_EQ(result.outcome, agent::RunOutcome::Finished) << result.error;
  const auto requests = model.Requests();
  ASSERT_EQ(requests.size(), 2u);
  EXPECT_LT(requests[0].size(), config.context_limit_tokens);
  EXPECT_LT(requests[1].size(), config.context_limit_tokens);
  EXPECT_EQ(requests[0].find("LATEST_REQUEST"), std::string::npos);
  EXPECT_NE(requests[0].find("past-signature"), std::string::npos);
  EXPECT_EQ(JsonString(requests[0], "/messages/3/content/1/id"), "past-call");
  EXPECT_EQ(JsonString(requests[0], "/messages/4/content/0/tool_use_id"),
            "past-call");
  EXPECT_EQ(messages[1].content[0].text, recent);
  EXPECT_EQ(JsonString(requests[1], "/messages/1/content/0/text"), recent);
}

TEST_F(ContextBudget, BoundedSummariesKeepParallelResultsWithSignedThinking) {
  config.context_limit_tokens = 10000;
  Add(std::string(5000, 'p'));
  agent::ContentBlock first, second;
  first.type = second.type = "tool_use";
  first.id = "first";
  second.id = "second";
  first.name = second.name = "inspect";
  const auto thinking =
      "[{\"type\":\"thinking\",\"thinking\":\"" + std::string(4500, 't') +
      R"(","signature":"parallel-signature"},{"type":"tool_use","id":"first","name":"inspect","input":{}},{"type":"tool_use","id":"second","name":"inspect","input":{}}])";
  messages.push_back({"assistant",
                      {first, second},
                      {},
                      thinking,
                      agent::ProviderAPI::AnthropicMessages});
  first.type = second.type = "tool_result";
  first.text = second.text = std::string(1000, 'x');
  for (const auto& block : {first, second})
    messages.push_back(
        {"user", {block}, {}, {}, agent::ProviderAPI::AnthropicMessages});
  Add(std::string(6000, 'r'));
  model.Queue(AnthropicText("first handoff"));
  model.Queue(AnthropicText("second handoff"));
  model.Queue(AnthropicText("done"));
  Run("system", {{"inspect", "Inspect", "{}"}});
  const auto requests = model.Requests();
  ASSERT_EQ(requests.size(), 3u);
  EXPECT_EQ(requests[0].find("parallel-signature"), std::string::npos);
  EXPECT_NE(requests[1].find("parallel-signature"), std::string::npos);
  EXPECT_EQ(JsonString(requests[1], "/messages/1/content/1/id"), "first");
  EXPECT_EQ(JsonString(requests[1], "/messages/1/content/2/id"), "second");
  EXPECT_EQ(JsonString(requests[1], "/messages/2/content/0/tool_use_id"),
            "first");
  EXPECT_EQ(JsonString(requests[1], "/messages/3/content/0/tool_use_id"),
            "second");
  for (const auto& request : requests)
    EXPECT_LT(request.size(), config.context_limit_tokens);
  EXPECT_EQ(messages[1].content[0].text, std::string(6000, 'r'));
}

TEST_F(ContextBudget, OneOversizedMessageFailsClearlyWithoutSendingOrRetrying) {
  config.context_limit_tokens = 10000;
  Add("Earlier history cannot make an oversized new request fit.");
  Add(std::string(12000, 'x'));
  const auto original = agent::SerializeMessages(messages);
  const auto result =
      agent::Runtime().Run(config, "system", messages, {}, {}, hooks, &context);
  EXPECT_EQ(result.outcome, agent::RunOutcome::Failed);
  EXPECT_EQ(result.error,
            "the conversation no longer fits the model's context");
  EXPECT_TRUE(model.Requests().empty());
  EXPECT_EQ(agent::SerializeMessages(messages), original);
}

TEST_F(ContextBudget, OversizedToolExchangeFailsAfterCappingWithoutRetrying) {
  config.context_limit_tokens = 10000;
  agent::ContentBlock call;
  call.type = "tool_use";
  call.id = "large-result";
  call.name = "inspect";
  messages.push_back(
      {"assistant", {call}, {}, {}, agent::ProviderAPI::AnthropicMessages});
  call.type = "tool_result";
  call.text = std::string(100000, 'x');
  messages.push_back(
      {"user", {call}, {}, {}, agent::ProviderAPI::AnthropicMessages});
  const auto original = agent::SerializeMessages(messages);
  const auto result =
      agent::Runtime().Run(config, "system", messages,
                           {{"inspect", "Inspect", "{}"}}, {}, hooks, &context);
  EXPECT_EQ(result.outcome, agent::RunOutcome::Failed);
  EXPECT_EQ(result.error,
            "the conversation no longer fits the model's context");
  EXPECT_TRUE(model.Requests().empty());
  EXPECT_EQ(agent::SerializeMessages(messages), original);
}

TEST(HostContextWindow, CatalogOverridesTheFallbackBudget) {
  TemporaryDirectory directory;
  host::HostStore home(directory.path);
  auto& db = home.db;
  auto insert = db.prepare("INSERT INTO model_lists VALUES(?,?,?)");
  insert.bind(1, "fixture")
      .bind(2, host::Models::CatalogConnection("fixture", "desktop"))
      .bind(3, R"([{"id":"fixture-model","contextWindowTokens":100000}])")
      .run();
  auto secrets = std::make_unique<MemorySecrets>();
  secrets->values["desktop:anthropic"] = "TEST_KEY";
  secrets->values["desktop:openai"] = "TEST_KEY";
  host::Settings settings(
      directory.path, [](const auto&, const auto&) {},
      [](const auto&, auto) { return ""; }, [](const auto&) { return true; },
      std::move(secrets), host::Settings::KeyStorage::Host);
  const auto fallback = settings.Resolve({}).context_limit_tokens;
  auto catalog = db.prepare("INSERT INTO model_lists VALUES(?,?,?)");
  catalog.bind(1, "anthropic")
      .bind(2, host::Models::CatalogConnection("anthropic", "desktop"))
      .bind(3, R"([{"id":"claude-sonnet-5-5","contextWindowTokens":100000}])")
      .run();
  const auto override_budget = settings.Resolve({}).context_limit_tokens;
  EXPECT_LT(override_budget, fallback);
  const auto config = settings.Resolve(
      {{"provider_id", Quote("fixture")}, {"model", Quote("fixture-model")}});
  EXPECT_EQ(config.context_limit_tokens, override_budget);
  EXPECT_EQ(host::Models::ContextWindowTokens(directory.path, "fixture",
                                              "fixture-model"),
            100000u);
}

TEST(HostContextWindow, ReportedWindowsBelongToTheSelectedConnection) {
  TemporaryDirectory directory;
  host::HostStore home(directory.path);
  auto secrets = std::make_unique<MemorySecrets>();
  secrets->values["desktop:anthropic"] = "TEST_KEY";
  host::Settings settings(
      directory.path, [](const auto&, const auto&) {},
      [](const auto&, auto) { return ""; }, [](const auto&) { return true; },
      std::move(secrets), host::Settings::KeyStorage::Host);
  for (const auto* provider : {"anthropic", "chatgpt"})
    for (const auto* profile : {"desktop", "web"})
      for (const auto* account : {"first", "second"}) {
        const auto window =
            std::string_view(account) == "first" ? 1000000u : 128000u;
        auto insert = home.db.prepare("INSERT INTO model_lists VALUES(?,?,?)");
        insert.bind(1, provider)
            .bind(2, host::Models::CatalogConnection(provider, profile, account,
                                                     account))
            .bind(3, "[{\"id\":\"shared-model\",\"contextWindowTokens\":" +
                         std::to_string(window) + "}]")
            .run();
        EXPECT_EQ(host::Models::ContextWindowTokens(directory.path, provider,
                                                    "shared-model", profile,
                                                    account, account),
                  window);
      }
  for (const auto* account : {"second", "first", "second"}) {
    const auto window =
        std::string_view(account) == "first" ? 1000000u : 128000u;
    EXPECT_EQ(settings
                  .Resolve({{"provider_id", Quote("anthropic")},
                            {"model", Quote("shared-model")},
                            {"key_profile", Quote(account)}})
                  .context_limit_tokens,
              window - 32768u - window / 20);
  }
  EXPECT_EQ(host::Models::ContextWindowTokens(directory.path, "anthropic",
                                              "shared-model", "other", "first",
                                              "first"),
            128000u);
  EXPECT_EQ(host::Models::CatalogConnection("chatgpt", "desktop"),
            host::Models::CatalogConnection("chatgpt", "desktop", "desktop"));
  EXPECT_EQ(host::Models::StoredChatGPTModels(directory.path),
            std::vector<std::string>{"shared-model"});
}

}  // namespace
}  // namespace tsugumi::test

namespace tsugumi::test {
TEST(HostSettingsStore,
     TrustedFoldersAreCanonicalAndRejectInvalidRegistrationAtomically) {
  TemporaryDirectory directory;
  const auto folder = directory.path / "trusted";
  std::filesystem::create_directory(folder);
  std::filesystem::create_directory_symlink(folder, directory.path / "link");
  host::Settings service(
      directory.path, [](const auto&, const auto&) {},
      [](const auto&, auto) { return ""; }, [](const auto&) { return true; },
      std::make_unique<MemorySecrets>());
  uint64_t revision = 0;
  auto update = [&](const std::string& raw,
                    const std::string& profile = "desktop") {
    H::RequestT request;
    H::SettingsUpdateRequestT value;
    value.profile = profile;
    value.revision = revision;
    auto item = std::make_unique<H::SettingT>();
    item->key = "agent.trustedFolders";
    item->value_json = raw;
    value.changes.push_back(std::move(item));
    request.value.Set(std::move(value));
    flatbuffers::FlatBufferBuilder builder;
    H::MessageT message;
    message.request_id = 1;
    message.payload.Set(std::move(request));
    H::FinishMessageBuffer(builder, H::Message::Pack(builder, &message));
    const auto reply = service.Handle(
        *H::GetMessage(builder.GetBufferPointer())->payload_as_Request(), 1);
    const auto* result = H::GetMessage(reply.data())->payload_as_Reply();
    if (const auto* settings = result->value_as_Settings()) {
      revision = settings->revision();
      return true;
    }
    return false;
  };
  EXPECT_TRUE(service.TrustedFolders().empty());
  EXPECT_TRUE(update("[" + Quote((directory.path / "link").string()) + "," +
                     Quote(folder.string()) + "]"));
  const std::vector<std::string> expected{
      std::filesystem::canonical(folder).string()};
  EXPECT_EQ(service.TrustedFolders(), expected);
  EXPECT_FALSE(update(R"(["relative"])"));
  EXPECT_FALSE(update("[" + Quote((directory.path / "absent").string()) + "]"));
  EXPECT_FALSE(update(R"([42])"));
  EXPECT_FALSE(update("[]", "web:test"));
  EXPECT_EQ(service.TrustedFolders(), expected);
  std::filesystem::rename(folder, directory.path / "moved");
  EXPECT_TRUE(update("[" + Quote(expected.front()) + "," +
                     Quote((directory.path / "moved").string()) + "]"));
  EXPECT_EQ(service.TrustedFolders().front(), expected.front());
  EXPECT_TRUE(update("[]"));
  EXPECT_TRUE(service.TrustedFolders().empty());
}
}  // namespace tsugumi::test
