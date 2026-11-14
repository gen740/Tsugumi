// The service uses the same explicit input/reply tables with a secret-store
// double. No platform Keychain, credential file or listener is involved.
#include <gtest/gtest.h>

#include "../agent_fixture_server.hh"
#include "host/settings.hh"

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
                                ->payload_as_Reply()->value_as_Settings()->values())
      if (item->key()->str() == "agent.animateWork")
        return item->value_json()->str();
    return std::string{};
  };
  auto update = [&](const std::string& raw) {
    const auto before = get();
    H::SettingsUpdateRequestT value;
    value.profile = "desktop";
    value.revision = H::GetMessage(before.data())
                         ->payload_as_Reply()->value_as_Settings()->revision();
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
  EXPECT_NE(H::GetMessage(invalid.data())->payload_as_Reply()->value_as_Failure(),
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
    Tsugumi::Service::ProviderInputT value;
    value.api_key = secret;
    const auto p = H::CreateProviderSetRequest(
        b, b.CreateString("desktop"), b.CreateString("anthropic"),
        Tsugumi::Service::ProviderInput::Pack(b, &value));
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
    Tsugumi::Service::ProviderInputT value;
    value.api_key = secret;
    const auto p = H::CreateProviderSetRequest(
        b, b.CreateString(profile), b.CreateString("anthropic"),
        Tsugumi::Service::ProviderInput::Pack(b, &value), false, true);
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
  Tsugumi::Service::ProviderInputT input;
  input.api_key = key;
  const auto value = H::CreateProviderSetRequest(
      b, b.CreateString("macos:keychain:fixture"), b.CreateString("anthropic"),
      Tsugumi::Service::ProviderInput::Pack(b, &input), false, true);
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
  const agent::json::Members selection{
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
  const auto update = [&](const std::string& mode, uint64_t revision, const std::string& key = "versions.noteMode") {
    flatbuffers::FlatBufferBuilder b;
    const auto change = H::CreateSetting(b, b.CreateString(key), b.CreateString(mode));
    const auto p = H::CreateSettingsUpdateRequest(b, b.CreateString("desktop"), revision,
        b.CreateVector(std::vector{change}));
    const auto request = H::CreateRequest(b, H::RequestPayload::SettingsUpdateRequest, p.Union());
    H::FinishMessageBuffer(b, H::CreateMessage(b, 1, H::Payload::Request, request.Union()));
    return service.Handle(*H::GetMessage(b.GetBufferPointer())->payload_as_Request(), 1);
  };
  for (const auto& invalid : {"true", "\"unknown\""}) {
    const auto failure = update(invalid, 0);
    ASSERT_TRUE(H::GetMessage(failure.data())->payload_as_Reply()->value_as_Failure());
  }
  auto reply = update("\"agent\"", 0);
  ASSERT_TRUE(H::GetMessage(reply.data())->payload_as_Reply()->value_as_Settings());
  EXPECT_EQ(service.VersionNoteMode("desktop"), "agent");
  EXPECT_EQ(service.VersionNoteMode("web:other"), "mechanical");
  reply = update("\"mechanical\"", 1);
  ASSERT_TRUE(H::GetMessage(reply.data())->payload_as_Reply()->value_as_Settings());
  EXPECT_EQ(service.VersionNoteMode("desktop"), "mechanical");
  EXPECT_EQ(service.VersionNoteProvider("desktop", "", "").credential, "ANTHROPIC_NOTE_KEY");
  EXPECT_EQ(service.VersionNoteProvider("desktop", "", "").model, "claude-haiku-5-5");
  EXPECT_EQ(service.VersionNoteProvider("desktop", "missing-platform-keys", "")
                .credential,
            "ANTHROPIC_NOTE_KEY");
  reply = update("\"openai\"", 2, "agent.provider");
  ASSERT_TRUE(H::GetMessage(reply.data())->payload_as_Reply()->value_as_Settings());
  EXPECT_EQ(service.VersionNoteProvider("desktop", "missing-anthropic-keys", "").credential, "OPENAI_NOTE_KEY");
  reply = update("\"custom-small-model\"", 3, "agent.thoughtModel.openai");
  ASSERT_TRUE(H::GetMessage(reply.data())->payload_as_Reply()->value_as_Settings());
  EXPECT_EQ(service.VersionNoteProvider("desktop", "", "").model, "custom-small-model");
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
      request.provider = std::make_unique<Tsugumi::Service::ProviderInputT>();
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
  request.provider = std::make_unique<Tsugumi::Service::ProviderInputT>();
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
  EXPECT_EQ(Status().storage, "agentd");
  EXPECT_EQ(Resolve().credential, "STORED_KEY");
  Provision("env", "ENV_KEY");
  EXPECT_EQ(Status().source, "env");
  EXPECT_EQ(Resolve().credential, "ENV_KEY");
  EXPECT_EQ(secrets->values["desktop:anthropic"], "STORED_KEY");
  Provision("env");
  EXPECT_EQ(Status().source, "agentd");
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
}  // namespace
}  // namespace tsugumi::test
