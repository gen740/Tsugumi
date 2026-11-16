#include "host/registry.hh"

#include <utility>

#include "host/failure.hh"
#include "host/projects.hh"
#include "util/fs.hh"

namespace tsugumi::host {
namespace {
auto Text(const flatbuffers::String* value) -> std::string {
  return value ? value->str() : "";
}
auto ValidateIdentity(const std::string& project,
                      const std::filesystem::path& path) -> void {
  const auto identity = fs::read_file(path / ".tsugumi/project-id");
  if (!identity || *identity != project)
    throw Failure(
        "stale_project",
        "This folder's project identity changed or was removed. Close and "
        "reopen this project to register its current identity. Conversation "
        "records have not been deleted.");
}
}  // namespace
ProjectRegistry::ProjectRegistry(StateGate& g, sql::Db& database,
                                 ConversationPath opened)
    : gate(g), db(database), conversation_path(std::move(opened)) {}

auto ProjectRegistry::ValidateLocation(const std::string& project,
                                       const std::filesystem::path& folder)
    -> void {
  const auto path = std::filesystem::canonical(folder);
  if (auto opened = conversation_path(project);
      opened && *opened != path / ".tsugumi")
    throw Failure(
        "conflict",
        "This project identity has conversations open in another folder");
  auto old = db.prepare("SELECT path FROM project_paths WHERE id=?");
  old.bind(1, project);
  if (old.step() && std::filesystem::is_directory(old.text(0)) &&
      std::filesystem::canonical(old.text(0)) != path)
    throw Failure("conflict",
                  "This project identity is registered in another folder");
  ValidateIdentity(project, path);
}
auto ProjectRegistry::Lookup(const std::string& id)
    -> ProjectRegistry::Project {
  Location(id);
  auto q = db.prepare("SELECT endpoint,token FROM projects WHERE id=?");
  q.bind(1, id);
  if (!q.step())
    throw Failure("not_found",
                  "Project is not registered with this core server");
  return {q.text(0), q.text(1)};
}
auto ProjectRegistry::Location(const std::string& project)
    -> std::optional<std::filesystem::path> {
  auto q = db.prepare("SELECT path FROM project_paths WHERE id=?");
  q.bind(1, project);
  if (!q.step()) return std::nullopt;
  const std::filesystem::path path(q.text(0));
  // Offline folders remain registered; a readable folder with a different
  // identity is stale and must never route RPC or conversation writes.
  if (std::filesystem::is_directory(path)) ValidateIdentity(project, path);
  return path;
}
auto ProjectRegistry::Locations() -> std::vector<std::string> {
  std::vector<std::string> result;
  auto q = db.prepare("SELECT id FROM project_paths ORDER BY id");
  while (q.step()) result.push_back(q.text(0));
  return result;
}
auto ProjectRegistry::Root(const std::string& project)
    -> std::filesystem::path {
  std::lock_guard lock(gate.mutex);
  Lookup(project);
  auto path = db.prepare("SELECT path FROM project_paths WHERE id=?");
  path.bind(1, project);
  if (!path.step() || path.text(0).empty())
    throw Failure("not_found", "Project path is not registered");
  return path.text(0);
}

// The current registration owns this canonical folder. Superseding registry
// metadata never deletes project files or conversation records.
auto ProjectRegistry::ReplacePath(const std::string& project,
                                  const std::filesystem::path& folder) -> void {
  const auto path = std::filesystem::canonical(folder);
  std::vector<std::string> replaced;
  auto old = db.prepare("SELECT id,path FROM project_paths WHERE id<>?");
  old.bind(1, project);
  while (old.step()) {
    std::error_code error;
    const auto previous = std::filesystem::weakly_canonical(old.text(1), error);
    if (!error && previous == path) replaced.push_back(old.text(0));
  }
  for (const auto& id : replaced) {
    for (const auto* table : {"projects", "project_paths"}) {
      auto remove =
          db.prepare(std::string("DELETE FROM ") + table + " WHERE id=?");
      remove.bind(1, id).run();
    }
    for (const auto* table :
         {"environment_projects", "project_attached_roots"}) {
      auto remove =
          db.prepare(std::string("DELETE FROM ") + table + " WHERE project=?");
      remove.bind(1, id).run();
    }
  }
  auto insert = db.prepare("INSERT OR REPLACE INTO project_paths VALUES(?,?)");
  insert.bind(1, project).bind(2, path.string()).run();
}
auto ProjectRegistry::RegisterProject(
    const Tsugumi::Host::RegisterProjectRequest& request) -> void {
  const auto endpoint = request.endpoint()->str();
  if (!endpoint.starts_with("unix:") && !endpoint.starts_with("ws://") &&
      !endpoint.starts_with("wss://"))
    throw Failure("invalid_request", "Unsupported project transport");
  if (request.path() && !request.path()->str().empty())
    ValidateLocation(request.project_id()->str(), request.path()->str());
  sql::Transaction transaction(db);
  auto managed = db.prepare("DELETE FROM environment_projects WHERE project=?");
  managed.bind(1, request.project_id()->str()).run();
  auto roots = db.prepare("DELETE FROM project_attached_roots WHERE project=?");
  roots.bind(1, request.project_id()->str()).run();
  auto q = db.prepare("INSERT OR REPLACE INTO projects VALUES(?,?,?,?)");
  q.bind(1, request.project_id()->str())
      .bind(2, endpoint)
      .bind(3, Text(request.token()))
      .bind(4, Text(request.title()))
      .run();
  if (request.path() && !request.path()->str().empty()) {
    ReplacePath(request.project_id()->str(), request.path()->str());
  }
  transaction.commit();
}
auto ProjectRegistry::RegisterFolder(
    const Tsugumi::Host::RegisterFolderRequest& request) -> void {
  Lookup(request.project_id()->str());
  if (!request.resource_id()->str().starts_with("folder:") ||
      request.folder()->str().empty())
    throw Failure("invalid_request",
                  "A folder resource needs a folder: ID and a path");
  auto old = db.prepare("SELECT project,path FROM folders WHERE id=?");
  old.bind(1, request.resource_id()->str());
  if (old.step() && (old.text(0) != request.project_id()->str() ||
                     old.text(1) != request.folder()->str()))
    throw Failure(
        "conflict",
        "A granted folder identity cannot be rebound to another path");
  auto q = db.prepare("INSERT OR IGNORE INTO folders VALUES(?,?,?,?)");
  q.bind(1, request.resource_id()->str())
      .bind(2, request.project_id()->str())
      .bind(3, request.folder()->str())
      .bind(4, Text(request.title()))
      .run();
}
auto ProjectRegistry::AttachedRoot(const std::string& project, const std::string& folder) -> std::string {
  // Explicit registrations are local. Managed environments have an exact map,
  // persisted with the endpoint so a hostd restart cannot select a guest's
  // same-named directory before the environment is observed again.
  auto managed = db.prepare("SELECT 1 FROM environment_projects WHERE project=?");
  managed.bind(1, project);
  if (!managed.step()) return folder;
  auto root = db.prepare(
      "SELECT execution_path FROM project_attached_roots WHERE project=? AND path=?");
  root.bind(1, project).bind(2, folder);
  if (!root.step())
    throw Failure("unavailable", "Attached folder is not mounted; restart the project environment");
  return root.text(0);
}
auto ProjectRegistry::RegisterReady(
    const Tsugumi::Host::EnvironmentStateT& state) -> void {
  ValidateLocation(state.project_id, state.project);
  sql::Transaction transaction(db);
  auto environment =
      db.prepare("INSERT OR REPLACE INTO environment_projects VALUES(?,?)");
  environment.bind(1, state.key).bind(2, state.project_id).run();
  const auto endpoint =
      state.unix_socket.empty() ? state.endpoint : "unix:" + state.unix_socket;
  auto q = db.prepare("INSERT OR REPLACE INTO projects VALUES(?,?,?,?)");
  q.bind(1, state.project_id)
      .bind(2, endpoint)
      .bind(3, state.token)
      .bind(4, host::ProjectDisplayName(state.project))
      .run();
  ReplacePath(state.project_id, state.project);
  auto clear = db.prepare("DELETE FROM project_attached_roots WHERE project=?");
  clear.bind(1, state.project_id).run();
  for (const auto& root : state.attached_roots) {
    auto insert = db.prepare("INSERT INTO project_attached_roots VALUES(?,?,?)");
    insert.bind(1, state.project_id).bind(2, root->host_path).bind(3, root->execution_path).run();
  }
  transaction.commit();
}
auto ProjectRegistry::UnregisterEnvironment(const std::string& key) -> void {
  auto q = db.prepare(
      "DELETE FROM projects WHERE id IN (SELECT project FROM "
      "environment_projects WHERE key=?)");
  q.bind(1, key).run();
}
}  // namespace tsugumi::host
