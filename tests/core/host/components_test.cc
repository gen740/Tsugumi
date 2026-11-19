#include <gtest/gtest.h>
#include <sys/stat.h>

#include <algorithm>
#include <future>

#include "../agent_fixture_server.hh"
#include "agent/providers.hh"
#include "agent/store.hh"
#include "host/access.hh"
#include "host/credentials.hh"
#include "host/demo_references.hh"
#include "host/failure.hh"
#include "host/registry.hh"
#include "host/settings.hh"
#include "host/state_gate.hh"
#include "host/store.hh"

namespace tsugumi::test {
TEST(HostComponents, HostStoreKeepsOnePrivateWalDatabaseForHostTransactions) {
  TemporaryDirectory directory;
  host::HostStore store(directory.path);
  struct stat mode{};
  ASSERT_EQ(::stat((directory.path / "host.sqlite").c_str(), &mode), 0);
  EXPECT_EQ(mode.st_mode & 0777, 0600);
  auto journal = store.db.prepare("PRAGMA journal_mode");
  ASSERT_TRUE(journal.step());
  EXPECT_EQ(journal.text(0), "wal");
  for (const auto* table :
       {"projects", "folders", "workspace_requests", "workspace_grants",
        "providers", "workspace_prompt_items", "approvals", "credentials",
        "environment_projects", "project_paths", "skill_catalogs",
        "conversation_locations", "model_lists", "setting_values",
        "settings_revision", "accounts", "provider_connections",
        "oauth_hosts"}) {
    auto query = store.db.prepare(
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?");
    query.bind(1, table);
    EXPECT_TRUE(query.step()) << table;
  }
  {
    sql::Transaction transaction(store.db);
    store.db.exec(
        "INSERT INTO projects VALUES('p','unix:/p','','P');"
        "INSERT INTO project_paths VALUES('p','/p');"
        "INSERT INTO environment_projects VALUES('env','p')");
  }
  for (const auto* table :
       {"projects", "project_paths", "environment_projects"}) {
    auto query = store.db.prepare(std::string("SELECT count(*) FROM ") + table);
    ASSERT_TRUE(query.step());
    EXPECT_EQ(query.i64(0), 0) << table;
  }
}
TEST(HostComponents, RetiringTheSettingsDatabaseLeavesOnlyNewKeysUsable) {
  TemporaryDirectory directory;
  const auto retired_path = directory.path / "host-settings.sqlite";
  {
    sql::Db retired(retired_path.string());
    retired.exec("CREATE TABLE secrets(id TEXT PRIMARY KEY, value TEXT);"
                 "INSERT INTO secrets VALUES('desktop:anthropic','old-key')");
  }
  host::HostStore store(directory.path);
  host::StateGate gate;
  host::Credentials credentials(gate, store.db, {});
  host::Settings settings(directory.path, credentials, nullptr,
                          host::Settings::KeyStorage::Host);
  const auto resolve = [&] {
    return settings.Resolve({{"provider_id", Quote("anthropic")}});
  };
  EXPECT_THROW(resolve(), std::runtime_error);
  ASSERT_TRUE(std::filesystem::remove(retired_path));
  store.db.exec("INSERT INTO secrets VALUES('desktop:anthropic','new-key')");
  EXPECT_EQ(resolve().credential, "new-key");
}
TEST(HostComponents, PendingApprovalsStayVisibleWithWorkspaceHistory) {
  namespace A = Tsugumi::Agent;
  TemporaryDirectory directory;
  host::HostStore store(directory.path);
  host::StateGate gate;
  host::ProjectRegistry registry(gate, store.db,
                                 [](const auto&) { return std::nullopt; });
  agent::ConversationStore conversations(
      gate, store.db, registry, [](auto value) { return value; },
      [](const auto&, auto) { return false; },
      [](const auto&) { return false; });
  bool stopping = false;
  host::Access access(
      gate, store.db, registry, conversations, stopping,
      {[](const auto&, const auto&) {}, [](const auto&, const auto&) {}});
  std::lock_guard lock(gate.mutex);
  store.db.exec(
      "WITH RECURSIVE ids(n) AS (VALUES(1) UNION ALL SELECT n+1 FROM ids "
      "WHERE n<501) INSERT INTO workspace_requests "
      "SELECT n,'project','conversation',0,'old request','denied' FROM ids");
  const auto approval =
      access.RequestApproval(host::ApprovalKind::Operation, "project",
                             "conversation", "call", "digest", "preview");
  flatbuffers::FlatBufferBuilder b;
  const auto value = A::CreateAccessListRequest(b);
  const auto request =
      A::CreateRequest(b, A::RequestPayload::AccessListRequest, value.Union());
  A::FinishMessageBuffer(
      b, A::CreateMessage(b, 1, A::Payload::Request, request.Union()));
  RequestIn in;
  in.request_id = 1;
  in.frame = std::make_shared<const std::string>(
      reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize());
  const auto reply = access.Handle(in, true);
  const auto* listed =
      A::GetMessage(reply.data())->payload_as_Reply()->value_as_AccessList();
  ASSERT_NE(listed, nullptr);
  ASSERT_NE(listed->requests(), nullptr);
  EXPECT_TRUE(std::ranges::any_of(*listed->requests(), [&](const auto* item) {
    return item->id()->str() == approval && item->state()->str() == "pending";
  }));
}

TEST(HostComponents, CredentialsKeepCachedTokensAndConcurrentRefreshRechecks) {
  TemporaryDirectory directory;
  host::HostStore store(directory.path);
  host::StateGate gate;
  std::stop_source shutdown;
  host::Credentials credentials(gate, store.db, shutdown.get_token());
  EXPECT_THROW(credentials.Get("absent"), host::Failure);
  credentials.Save(
      "account",
      ::tsugumi::json::Object(
          R"({"access_token":"current","refresh_token":"refresh","id_token":"identity","expires_at_ms":9999999999999,"scope":"chatgpt.tokens.use.direct"})"));
  EXPECT_EQ(credentials.AccessToken("account", ""), "current");
  // Another caller already replaced the rejected token: return the saved one
  // even if expired, exactly as the original refresh recheck did.
  auto expired = credentials.Get("account");
  expired["expires_at_ms"] = "0";
  credentials.Save("account", expired);
  EXPECT_EQ(credentials.AccessToken("account", "previous"), "current");
  const auto secrets = credentials.Secrets();
  EXPECT_EQ(secrets,
            (std::vector<std::string>{"current", "refresh", "identity"}));
  {
    std::lock_guard lock(gate.mutex);
    EXPECT_EQ(credentials.Get("account"), expired);
  }
}
class CredentialRefreshTest : public testing::Test {
 protected:
  TemporaryDirectory directory;
  host::HostStore store{directory.path};
  host::StateGate gate;
  ModelServer endpoint;
  host::Credentials credentials{gate, store.db, {}};
  host::Settings settings{directory.path, credentials, nullptr,
                          host::Settings::KeyStorage::Host};
  std::function<std::string(std::string_view)> access_token;

  CredentialRefreshTest() {
    auto value = ::tsugumi::json::Object(
        R"({"access_token":"old-access","refresh_token":"old-refresh","id_token":"old-id","scope":"openid chatgpt.tokens.use.direct","expires_at_ms":0,"resource":"https://api.openai.com/v1"})");
    value["token_endpoint"] = Quote(endpoint.URL());
    credentials.Save("account", value);
    store.db.exec(
        R"(INSERT INTO accounts VALUES('desktop','account','{"active":true,"signed_in":true,"plan_enabled":true}'))");
    access_token =
        settings.Resolve({{"provider_id", Quote("chatgpt")}}).access_token;
  }
  auto Account() -> ::tsugumi::json::Members {
    auto q =
        store.db.prepare("SELECT json FROM accounts WHERE id='account'");
    if (!q.step()) throw std::runtime_error("missing test account");
    return ::tsugumi::json::Object(q.text(0));
  }
  auto Status() -> std::unique_ptr<Tsugumi::Host::ProviderStatusT> {
    namespace H = Tsugumi::Host;
    flatbuffers::FlatBufferBuilder b;
    auto value = H::CreateProviderStatusRequest(b, b.CreateString("desktop"),
                                                b.CreateString("chatgpt"));
    auto request = H::CreateRequest(b, H::RequestPayload::ProviderStatusRequest,
                                    value.Union());
    H::FinishMessageBuffer(
        b, H::CreateMessage(b, 1, H::Payload::Request, request.Union()));
    const auto reply = settings.Handle(
        *H::GetMessage(b.GetBufferPointer())->payload_as_Request(), 1);
    return std::unique_ptr<H::ProviderStatusT>(H::GetMessage(reply.data())
                                                   ->payload_as_Reply()
                                                   ->value_as_ProviderStatus()
                                                   ->UnPack());
  }
};
TEST_F(CredentialRefreshTest,
       NarrowedScopePersistsAllRotatedTokensBeforeRefusal) {
  endpoint.QueueHTTP(
      R"({"access_token":"rotated-access","refresh_token":"rotated-refresh","id_token":"rotated-id","expires_in":7200,"scope":"openid"})");
  EXPECT_THROW(access_token(""), agent::ProviderError);
  const auto saved = credentials.Get("account");
  EXPECT_EQ(::tsugumi::json::Text(saved, "access_token"), "rotated-access");
  EXPECT_EQ(::tsugumi::json::Text(saved, "refresh_token"), "rotated-refresh");
  EXPECT_EQ(::tsugumi::json::Text(saved, "id_token"), "rotated-id");
  EXPECT_GT(::tsugumi::json::UInt(::tsugumi::json::Get(saved, "expires_at_ms")),
            0u);
  EXPECT_EQ(::tsugumi::json::Text(saved, "scope"), "openid");
  EXPECT_TRUE(
      ::tsugumi::json::Bool(::tsugumi::json::Get(Account(), "signed_in")));
  EXPECT_FALSE(
      ::tsugumi::json::Bool(::tsugumi::json::Get(Account(), "plan_enabled")));
  EXPECT_FALSE(Status()->configured);
  EXPECT_FALSE(Status()->message.empty());
  EXPECT_THROW(access_token(""), agent::ProviderError);
  EXPECT_EQ(endpoint.Requests().size(), 1u);
}
TEST_F(CredentialRefreshTest,
       OmittedScopeInheritsAndUsesReplacementOnNextRefresh) {
  endpoint.QueueHTTP(
      R"({"access_token":"rotated-access","refresh_token":"rotated-refresh","expires_in":0})");
  EXPECT_EQ(access_token(""), "rotated-access");
  EXPECT_EQ(::tsugumi::json::Text(credentials.Get("account"), "scope"),
            "openid chatgpt.tokens.use.direct");
  endpoint.QueueHTTP(R"({"access_token":"latest-access","expires_in":3600})");
  EXPECT_EQ(access_token(""), "latest-access");
  ASSERT_EQ(endpoint.Requests().size(), 2u);
  EXPECT_NE(endpoint.Requests()[1].find("refresh_token=rotated-refresh"),
            std::string::npos);
  EXPECT_EQ(::tsugumi::json::Text(credentials.Get("account"), "id_token"),
            "old-id");
  EXPECT_TRUE(Status()->configured);
}
TEST_F(CredentialRefreshTest, ExplicitEmptyScopeDisablesPlanAndKeepsTokens) {
  endpoint.QueueHTTP(
      R"({"access_token":"rotated-access","refresh_token":"rotated-refresh","scope":""})");
  EXPECT_THROW(access_token(""), agent::ProviderError);
  EXPECT_EQ(::tsugumi::json::Text(credentials.Get("account"), "refresh_token"),
            "rotated-refresh");
  EXPECT_EQ(::tsugumi::json::Text(credentials.Get("account"), "scope"), "");
  EXPECT_FALSE(Status()->configured);
}
TEST_F(CredentialRefreshTest,
       TerminalCodesRequireSignInAndNeverRetryConsumedTokens) {
  for (const auto* code :
       {"invalid_grant", "invalid_refresh_token", "token_expired",
        "refresh_token_expired", "refresh_token_invalidated",
        "refresh_token_reused", "refresh_token_revoked"}) {
    SCOPED_TRACE(code);
    auto value = credentials.Get("account");
    value["refresh_token"] = Quote("old-refresh");
    credentials.Save("account", value);
    endpoint.QueueHTTP("{\"error\":{\"code\":" + Quote(code) +
                           ",\"message\":\"old-refresh\"}}",
                       400);
    testing::internal::CaptureStderr();
    try {
      access_token("");
      ADD_FAILURE() << "Terminal refresh was accepted";
    } catch (const agent::ProviderError& error) {
      EXPECT_FALSE(std::string(error.what()).empty());
      EXPECT_EQ(error.code, code);
      EXPECT_FALSE(error.retryable);
    }
    const auto log = testing::internal::GetCapturedStderr();
    EXPECT_EQ(log.find("old-refresh"), std::string::npos);
    EXPECT_NE(log.find(code), std::string::npos);
    EXPECT_FALSE(
        ::tsugumi::json::Bool(::tsugumi::json::Get(Account(), "signed_in")));
    EXPECT_FALSE(Status()->configured);
    EXPECT_FALSE(Status()->message.empty());
    const auto requests = endpoint.Requests().size();
    EXPECT_THROW(access_token(""), agent::ProviderError);
    EXPECT_EQ(endpoint.Requests().size(), requests);
  }
}
TEST_F(CredentialRefreshTest, RefreshFailureDoesNotOverwriteNewSignIn) {
  auto refresh = std::async(std::launch::async, [&] {
    EXPECT_THROW(access_token(""), agent::ProviderError);
  });
  EXPECT_TRUE(endpoint.WaitForRequests(1, std::chrono::seconds(10)));
  auto sign_in = std::async(std::launch::async, [&] {
    std::lock_guard operation(credentials.credential_mutex);
    std::lock_guard lock(gate.mutex);
    sql::Transaction tx(store.db);
    EXPECT_FALSE(Status()->configured);
    auto value = credentials.Get("account");
    value["access_token"] = Quote("new-access");
    value["refresh_token"] = Quote("new-refresh");
    value["expires_at_ms"] = "9999999999999";
    credentials.Save("account", value);
    store.db.exec(
        "UPDATE accounts SET json=json_set(json,'$.signed_in',json('true'))");
    tx.commit();
  });
  endpoint.QueueHTTP(R"({"error":"invalid_grant"})", 400);
  refresh.get();
  sign_in.get();
  EXPECT_TRUE(Status()->configured);
  EXPECT_EQ(access_token(""), "new-access");
}
TEST_F(CredentialRefreshTest,
       TemporaryFailuresKeepCredentialsAndConfiguration) {
  const auto before = credentials.Get("account");
  for (const auto status : {503, 429}) {
    endpoint.QueueHTTP(
        R"({"error":"invalid_grant","error_description":"temporarily unavailable old-refresh"})",
        status);
    try {
      access_token("");
      ADD_FAILURE() << "Temporary refresh was accepted";
    } catch (const agent::ProviderError& error) {
      EXPECT_TRUE(error.retryable);
      EXPECT_EQ(error.status, status);
    }
    EXPECT_EQ(credentials.Get("account"), before);
    EXPECT_TRUE(Status()->configured);
    EXPECT_TRUE(
        ::tsugumi::json::Bool(::tsugumi::json::Get(Account(), "signed_in")));
  }
  endpoint.QueueHTTP(
      R"({"access_token":"recovered","refresh_token":"replacement"})");
  EXPECT_EQ(access_token(""), "recovered");
}
TEST(HostComponents, DemoReferencesKeepTheConfiguredRootAndReadLimit) {
  host::DemoReferences absent({});
  EXPECT_EQ(absent.List(), "[]");
  EXPECT_THROW(absent.Read("example.md"), host::Failure);
  TemporaryDirectory directory;
  Write(directory.path / "example.md", "reference");
  host::DemoReferences demos(directory.path);
  EXPECT_EQ(demos.ListFiles(), R"(["example.md"])");
  Write(
      directory.path / "ja/index.json",
      R"({"demos":[{"path":"sample","title":"Sample","description":"One line"}]})");
  EXPECT_EQ(
      demos.List(),
      R"([{"description":"One line","name":"Sample","path":"ja/sample"}])");
  EXPECT_EQ(demos.Read("example.md"), "reference");
  EXPECT_THROW(demos.Read("../example.md"), host::Failure);
  EXPECT_THROW(demos.Read("/example.md"), host::Failure);
  Write(directory.path / "boundary.md", std::string(1u << 20, 'x'));
  EXPECT_EQ(demos.Read("boundary.md").size(), 1u << 20);
  Write(directory.path / "large.md", std::string((1u << 20) + 1, 'x'));
  try {
    demos.Read("large.md");
    FAIL() << "Oversized demo was accepted";
  } catch (const host::Failure& error) {
    EXPECT_EQ(error.code, "limit_exceeded");
  }
}
TEST(HostComponents, ProvidersKeepConversationSelectionsAndHelperProfiles) {
  TemporaryDirectory directory;
  host::HostStore store(directory.path);
  host::StateGate gate;
  host::Credentials credentials{gate, store.db, {}};
  host::Settings settings(directory.path, credentials, nullptr,
                          host::Settings::KeyStorage::Host);
  agent::Providers providers(gate, store.db, settings);
  std::lock_guard lock(gate.mutex);
  EXPECT_TRUE(providers.Selection("absent").empty());
  EXPECT_EQ(providers.Profile("absent"), "desktop");
  const auto selection = ::tsugumi::json::Object(
      R"({"profile":"alternate","provider_id":"fixture","model":"selected"})");
  providers.Configure("conversation", ::tsugumi::json::Encode(selection));
  EXPECT_EQ(providers.Selection("conversation"), selection);
  EXPECT_EQ(providers.Profile("conversation"), "alternate");
  const auto config = providers.ForConversation("conversation");
  EXPECT_EQ(config.auth, agent::ProviderAuth::None);
  EXPECT_EQ(config.model, "selected");
  providers.Configure("other", R"({"provider_id":"fixture"})");
  EXPECT_EQ(providers.Profile("other"), "desktop");
  EXPECT_EQ(providers.ForConversation("other").model, "fixture");
  EXPECT_EQ(providers.Selection("conversation"), selection);
}
TEST(HostComponents,
     RegistryKeepsEnvironmentRegistrationAtomicAndRetainsLocations) {
  TemporaryDirectory directory;
  const auto root = directory.path / "project";
  const auto other = directory.path / "other";
  Write(root / ".tsugumi/project-id", "project");
  Write(other / ".tsugumi/project-id", "other");
  host::HostStore store(directory.path);
  host::StateGate gate;
  host::ProjectRegistry registry(
      gate, store.db,
      [](const std::string&) -> std::optional<std::filesystem::path> {
        return std::nullopt;
      });
  std::lock_guard lock(gate.mutex);
  Tsugumi::Host::EnvironmentStateT ready;
  ready.key = "env";
  ready.project_id = "project";
  ready.project = root.string();
  ready.endpoint = "ws://localhost:1/rpc";
  ready.unix_socket = "/tmp/project.sock";
  ready.token = "capability";
  auto attached = std::make_unique<Tsugumi::Host::AttachedRootT>();
  attached->host_path = "/etc";
  attached->execution_path = (directory.path / "mounted-mac-etc").string();
  const auto mounted = attached->execution_path;
  ready.attached_roots.push_back(std::move(attached));
  registry.RegisterReady(ready);
  EXPECT_EQ(registry.AttachedRoot("project", "/etc"), mounted);
  host::ProjectRegistry restored(gate, store.db, {});
  EXPECT_EQ(restored.AttachedRoot("project", "/etc"), mounted);
  EXPECT_THROW(registry.AttachedRoot("project", "/unmounted"), host::Failure);
  EXPECT_EQ(registry.Lookup("project").endpoint, "unix:/tmp/project.sock");
  EXPECT_EQ(registry.Lookup("project").token, "capability");
  EXPECT_EQ(registry.Root("project"), root);
  store.db.exec(
      "CREATE TRIGGER reject_location BEFORE INSERT ON project_paths "
      "WHEN NEW.id='other' BEGIN SELECT RAISE(ABORT,'fixture'); END");
  auto failed = ready;
  failed.key = "failed";
  failed.project_id = "other";
  failed.project = other.string();
  EXPECT_THROW(registry.RegisterReady(failed), sql::Error);
  EXPECT_THROW(registry.Lookup("other"), host::Failure);
  auto absent =
      store.db.prepare("SELECT 1 FROM environment_projects WHERE key='failed'");
  EXPECT_FALSE(absent.step());
  registry.UnregisterEnvironment("env");
  EXPECT_THROW(registry.Lookup("project"), host::Failure);
  auto location =
      store.db.prepare("SELECT path FROM project_paths WHERE id='project'");
  ASSERT_TRUE(location.step());
  EXPECT_EQ(location.text(0), root.string());
}
TEST(HostComponents, RegistryReplacesCanonicalPathAtRegistration) {
  namespace H = Tsugumi::Host;
  TemporaryDirectory directory;
  const auto root = directory.path / "project";
  Write(root / ".tsugumi/project-id", "old");
  host::HostStore store(directory.path);
  host::StateGate gate;
  host::ProjectRegistry registry(gate, store.db, [](const auto&) {
    return std::optional<std::filesystem::path>{};
  });
  const auto register_project = [&](const std::string& id,
                                    const std::filesystem::path& path) {
    H::RegisterProjectRequestT request;
    request.project_id = id;
    request.endpoint = "unix:/project";
    request.path = path.string();
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(H::RegisterProjectRequest::Pack(builder, &request));
    registry.RegisterProject(*flatbuffers::GetRoot<H::RegisterProjectRequest>(
        builder.GetBufferPointer()));
  };
  register_project("old", root);
  Write(root / ".tsugumi/project-id", "new");
  host::ProjectRegistry restored(gate, store.db, {});
  EXPECT_NO_THROW(restored.Lookup("old"));
  const auto alias = directory.path / "alias";
  std::filesystem::create_directory_symlink(root, alias);
  register_project("new", alias);
  EXPECT_EQ(restored.Locations(), std::vector<std::string>{"new"});
  EXPECT_EQ(restored.Root("new"), std::filesystem::canonical(root));
  EXPECT_THROW(restored.Lookup("old"), host::Failure);
}

TEST(HostComponents,
     RegistryKeepsProjectAndFolderIdentitiesBoundToTheirLocations) {
  namespace H = Tsugumi::Host;
  TemporaryDirectory directory;
  const auto root = directory.path / "project";
  const auto other = directory.path / "other";
  Write(root / ".tsugumi/project-id", "project");
  Write(other / ".tsugumi/project-id", "other");
  host::HostStore store(directory.path);
  host::StateGate gate;
  std::optional<std::filesystem::path> opened;
  host::ProjectRegistry registry(gate, store.db,
                                 [&](const std::string&) { return opened; });
  std::lock_guard lock(gate.mutex);
  const auto register_project = [&](const H::RegisterProjectRequestT& input) {
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(H::RegisterProjectRequest::Pack(builder, &input));
    registry.RegisterProject(*flatbuffers::GetRoot<H::RegisterProjectRequest>(
        builder.GetBufferPointer()));
  };
  H::RegisterProjectRequestT project;
  project.project_id = "project";
  project.endpoint = "http://unsupported";
  project.path = root.string();
  EXPECT_THROW(register_project(project), host::Failure);
  project.endpoint = "unix:/project";
  project.token = "secret";
  EXPECT_NO_THROW(register_project(project));
  EXPECT_EQ(registry.Lookup("project").token, "secret");
  project.path = other.string();
  EXPECT_THROW(register_project(project), host::Failure);
  opened = other / ".tsugumi";
  EXPECT_THROW(registry.ReserveLocation("project", root), host::Failure);
  opened.reset();
  const auto register_folder = [&](const H::RegisterFolderRequestT& input) {
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(H::RegisterFolderRequest::Pack(builder, &input));
    registry.RegisterFolder(*flatbuffers::GetRoot<H::RegisterFolderRequest>(
        builder.GetBufferPointer()));
  };
  H::RegisterFolderRequestT folder;
  folder.project_id = "project";
  folder.resource_id = "folder:attached";
  folder.folder = root.string();
  EXPECT_NO_THROW(register_folder(folder));
  EXPECT_NO_THROW(register_folder(folder));
  folder.folder = other.string();
  EXPECT_THROW(register_folder(folder), host::Failure);
}
}  // namespace tsugumi::test
