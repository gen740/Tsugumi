#include <gtest/gtest.h>
#include <sys/stat.h>

#include "../agent_fixture_server.hh"
#include "agent/providers.hh"
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
       {"projects", "folders", "access_requests", "grants", "providers",
        "save_preparations", "access_prompt_items", "operation_approvals",
        "credentials", "conversation_locations", "environment_projects",
        "project_paths", "skill_catalogs", "model_lists"}) {
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
TEST(HostComponents, StateGateRetainsRecursiveLocking) {
  host::StateGate gate;
  std::lock_guard outer(gate.mutex);
  std::unique_lock inner(gate.mutex, std::try_to_lock);
  EXPECT_TRUE(inner.owns_lock());
}
TEST(HostComponents, CredentialsKeepCachedTokensAndConcurrentRefreshRechecks) {
  TemporaryDirectory directory;
  host::HostStore store(directory.path);
  host::StateGate gate;
  std::stop_source shutdown;
  host::Credentials credentials(gate, store.db, shutdown.get_token());
  EXPECT_THROW(credentials.Get("absent"), host::Failure);
  credentials.Import(
      "account",
      agent::json::Object(
          R"({"access_token":"current","refresh_token":"refresh","id_token":"identity","expires_at_ms":9999999999999,"scope":"metadata"})"));
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
TEST(HostComponents, DemoReferencesKeepTheConfiguredRootAndReadLimit) {
  host::DemoReferences absent({});
  EXPECT_EQ(absent.List(), "[]");
  EXPECT_THROW(absent.Read("example.md"), host::Failure);
  TemporaryDirectory directory;
  Write(directory.path / "example.md", "reference");
  host::DemoReferences demos(directory.path);
  EXPECT_EQ(demos.List(), R"(["example.md"])");
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
  host::Settings settings(
      directory.path, [](const auto&, const auto&) {},
      [](const auto&, auto) { return ""; }, [](const auto&) { return true; },
      nullptr, host::Settings::KeyStorage::Host);
  agent::Providers providers(gate, store.db, settings);
  std::lock_guard lock(gate.mutex);
  EXPECT_TRUE(providers.Selection("absent").empty());
  EXPECT_EQ(providers.Profile("absent"), "desktop");
  const auto selection = agent::json::Object(
      R"({"profile":"alternate","provider_id":"fixture","model":"selected"})");
  providers.Configure("conversation", agent::json::Encode(selection));
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
TEST(HostComponents, RegistryReplacesCanonicalPathAndRejectsLateOldIdentity) {
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
  // Read-time validation marks persisted registrations stale before a new
  // registration arrives, including after restarting the registry.
  host::ProjectRegistry restored(gate, store.db, {});
  try {
    restored.Lookup("old");
    FAIL() << "Stale registration was accepted";
  } catch (const host::Failure& error) {
    EXPECT_EQ(error.code, "stale_project");
    EXPECT_NE(std::string(error.what()).find("Close and reopen"),
              std::string::npos);
  }
  const auto alias = directory.path / "alias";
  std::filesystem::create_directory_symlink(root, alias);
  register_project("new", alias);
  EXPECT_EQ(restored.Locations(), std::vector<std::string>{"new"});
  EXPECT_EQ(restored.Root("new"), std::filesystem::canonical(root));
  EXPECT_THROW(register_project("old", root), host::Failure);
  EXPECT_EQ(restored.Locations(), std::vector<std::string>{"new"});
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
  EXPECT_THROW(registry.ValidateLocation("project", root), host::Failure);
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
