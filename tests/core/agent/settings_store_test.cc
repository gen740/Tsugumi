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
      std::move(memory));
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
  EXPECT_THROW(
      service.Resolve({{"profile", Quote("desktop")},
                       {"key_profile", Quote("macos:keychain:missing")}}),
      std::runtime_error);
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
        [](const auto&, auto) { return ""; }, [](const auto&) { return true; });
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
