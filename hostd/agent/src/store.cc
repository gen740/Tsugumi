#include "agent/store.hh"

#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <utility>

#include "agent/context.hh"
#include "agent/wire.hh"
#include "host/failure.hh"
#include "host/identifiers.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/json_object.hh"
#include "tsugumi/util/log.hh"

namespace tsugumi::agent {
namespace A = Tsugumi::Agent;
using host::Failure;
namespace {
auto Text(const flatbuffers::String* value) -> std::string {
  return value ? value->str() : "";
}
auto Now() -> int64_t {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
}  // namespace

// Conversation records contain no provisioning, host capabilities or settings.
auto CreateConversationTables(sql::Db& db) -> void {
  db.exec(
      "CREATE TABLE IF NOT EXISTS call_counters(conversation TEXT NOT "
      "NULL, scope TEXT NOT NULL, value INTEGER NOT NULL, PRIMARY "
      "KEY(conversation,scope))");
  db.exec(
      "CREATE TABLE IF NOT EXISTS conversations(id TEXT PRIMARY KEY, project "
      "TEXT NOT NULL, role TEXT NOT NULL, title TEXT NOT NULL, messages TEXT "
      "NOT NULL, updated INTEGER NOT NULL, owner TEXT NOT NULL);"
      "CREATE TABLE IF NOT EXISTS conversation_resources(conversation TEXT "
      "NOT NULL, project TEXT NOT NULL, PRIMARY KEY(conversation,project));"
      "CREATE TABLE IF NOT EXISTS archives(conversation TEXT NOT NULL, "
      "messages TEXT NOT NULL);"
      "CREATE TABLE IF NOT EXISTS usage(conversation TEXT PRIMARY KEY, json "
      "TEXT NOT NULL);"
      "CREATE TABLE IF NOT EXISTS events(seq INTEGER PRIMARY KEY "
      "AUTOINCREMENT, conversation TEXT NOT NULL, json TEXT NOT NULL);"
      "CREATE INDEX IF NOT EXISTS events_conversation ON "
      "events(conversation,seq);"
      "CREATE TABLE IF NOT EXISTS conversation_turns(conversation TEXT NOT "
      "NULL, id TEXT "
      "NOT NULL, digest TEXT NOT NULL, state TEXT NOT NULL, action INTEGER "
      "NOT NULL, operation TEXT NOT NULL, frame BLOB, message_index INTEGER, "
      "approval_digest TEXT, "
      "PRIMARY KEY(conversation,id));"
      "CREATE TABLE IF NOT EXISTS receipts(conversation TEXT NOT NULL, id "
      "TEXT NOT NULL, digest TEXT NOT NULL, state TEXT NOT NULL, result TEXT "
      "NOT NULL, error INTEGER NOT NULL, project TEXT NOT NULL, operation "
      "TEXT NOT NULL, PRIMARY KEY(conversation,id));");
  db.exec(
      "CREATE TABLE IF NOT EXISTS model_bindings(conversation TEXT PRIMARY "
      "KEY, "
      "provider TEXT NOT NULL, model TEXT NOT NULL, effort TEXT NOT NULL, fast "
      "INTEGER NOT NULL)");
  db.exec(
      "CREATE TABLE IF NOT EXISTS helper_delegations(conversation TEXT NOT "
      "NULL, "
      "parent TEXT NOT NULL, call TEXT NOT NULL, count INTEGER NOT NULL, "
      "digest TEXT NOT NULL, PRIMARY KEY(conversation,parent,call))");
  db.exec(
      "CREATE TABLE IF NOT EXISTS helpers(conversation TEXT NOT NULL, "
      "parent_call TEXT NOT NULL, task INTEGER NOT NULL, digest TEXT NOT "
      "NULL, messages TEXT NOT NULL, result TEXT NOT NULL, state TEXT "
      "NOT NULL, spec TEXT NOT NULL, anchor INTEGER NOT NULL, archive TEXT "
      "NOT NULL, PRIMARY KEY(conversation,parent_call,task))");
}

struct ConversationStore::ProjectStore {
  std::filesystem::path path;
  host::ProcessLock lock;
  sql::Db db;
  ProjectStore(std::filesystem::path p, host::ProcessLock l);
};
ConversationStore::ProjectStore::ProjectStore(std::filesystem::path p,
                                              host::ProcessLock l)
    : path(std::move(p)),
      lock(std::move(l)),
      db((path / "agent.sqlite").string(),
         sql::FormatVersion{kAgentFormatVersion}) {
  ::chmod((path / "agent.sqlite").c_str(), 0600);
  // hostd writes the original host folder, never the VM's virtiofs mount.
  // DELETE keeps a quiescent project copy self-contained (no WAL sidecar).
  db.exec("PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;");
  db.initialize_format(kAgentFormatVersion,
                       [&] { CreateConversationTables(db); });
}

ConversationStore::ConversationStore(host::StateGate& g, sql::Db& home,
                                     host::ProjectRegistry& projects,
                                     Scrub redact, HasGrant grant,
                                     IsRunning busy, std::string app)
    : gate(g),
      db(home),
      registry(projects),
      scrub(std::move(redact)),
      has_grant(std::move(grant)),
      is_running(std::move(busy)),
      application(std::move(app)) {
  db.exec(
      "UPDATE conversation_turns SET state='interrupted' WHERE "
      "state='running'");
}
ConversationStore::~ConversationStore() = default;
auto ConversationStore::OpenedPath(const std::string& project)
    -> std::optional<std::filesystem::path> {
  if (auto found = project_stores.find(project); found != project_stores.end())
    return found->second->path;
  return std::nullopt;
}

auto ConversationStore::ProjectDatabase(const std::string& project, bool create)
    -> sql::Db* {
  if (project.empty()) return &db;
  const auto location = registry.Location(project);
  if (!location) {
    if (create) throw Failure("unavailable", "The project has no host folder");
    return nullptr;
  }
  const std::filesystem::path root = *location;
  if (!std::filesystem::is_directory(root)) {
    if (create)
      throw Failure("unavailable", "The project folder is unavailable");
    return nullptr;
  }
  const auto path = std::filesystem::canonical(root) / ".tsugumi";
  if (auto found = project_stores.find(project);
      found != project_stores.end()) {
    if (found->second->path != path)
      throw Failure("conflict",
                    "This project identity is already open in another folder");
    return &found->second->db;
  }
  if (!create && !std::filesystem::exists(path / "agent.sqlite"))
    return nullptr;
  std::filesystem::create_directories(path);
  // Do not follow project-supplied links out of its state folder.
  for (const auto& entry :
       {path, path / "agent.sqlite", path / "agent.lock",
        path / "agent.sqlite-journal", path / "agent.sqlite-wal",
        path / "agent.sqlite-shm"})
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(entry)))
      throw Failure("invalid_request",
                    "Agent state must not be a symbolic link");
  // A newly registered identity can replace a folder whose old database was
  // open here. Release that obsolete handle before locking the current state.
  std::erase_if(project_stores, [&](const auto& entry) {
    return entry.first != project && entry.second->path == path;
  });
  std::optional<host::LockHolder> holder;
  auto lock =
      host::ProcessLock::TryAcquire(path / "agent.lock", application, holder);
  if (!lock) throw host::ConversationLockFailure(std::move(holder));
  auto store = std::make_unique<ProjectStore>(path, std::move(*lock));
  auto identity =
      store->db.prepare("SELECT 1 FROM conversations WHERE project<>? LIMIT 1");
  identity.bind(1, project);
  if (identity.step())
    throw Failure(
        "conversation_identity_mismatch",
        "These conversation records belong to another project identity. "
        "Restore the matching .tsugumi folder from a backup. To start new "
        "conversations, close the project, move agent.sqlite out of .tsugumi "
        "to preserve it, then reopen the project. No records were deleted.");
  store->db.exec(
      "UPDATE conversation_turns SET state='interrupted' WHERE "
      "state='running'");
  auto* result = &store->db;
  project_stores.emplace(project, std::move(store));
  return result;
}

auto ConversationStore::Databases(const std::string& project)
    -> std::vector<sql::Db*> {
  if (!project.empty()) {
    if (auto* store = ProjectDatabase(project)) return {store};
    return {};
  }
  std::vector<sql::Db*> result{&db};
  for (const auto& id : registry.Locations()) {
    try {
      if (auto* store = ProjectDatabase(id)) result.push_back(store);
    } catch (const std::exception& error) {
      log::warn("Cannot open conversations for project " + id + ": " +
                error.what());
    }
  }
  return result;
}

auto ConversationStore::FindConversationDatabase(
    const std::string& id, const std::optional<std::string>& project)
    -> sql::Db* {
  auto contains = [&](sql::Db& store) {
    auto q = store.prepare("SELECT project FROM conversations WHERE id=?");
    q.bind(1, id);
    if (!q.step()) return false;
    RememberConversation(id, q.text(0));
    return true;
  };
  if (project) {
    if (auto* store = ProjectDatabase(*project); store && contains(*store))
      return store;
    return nullptr;
  }
  if (const auto known = ConversationProject(id)) {
    auto* store = ProjectDatabase(*known);
    return store && contains(*store) ? store : nullptr;
  }
  sql::Db* result = nullptr;
  for (auto* store : Databases())
    if (contains(*store)) {
      if (result) {
        throw Failure("conflict",
                      "Conversation ID exists in multiple projects");
      }
      result = store;
    }
  return result;
}

// Routing metadata belongs to the host, so a known ID can be resolved after a
// restart without opening any unrelated project's conversation records.
auto ConversationStore::ConversationProject(const std::string& id)
    -> std::optional<std::string> {
  auto q = db.prepare(
      "SELECT project FROM conversation_locations WHERE conversation=?");
  q.bind(1, id);
  if (!q.step()) return std::nullopt;
  return q.text(0);
}

auto ConversationStore::RememberConversation(const std::string& id,
                                             const std::string& project)
    -> void {
  if (const auto known = ConversationProject(id)) {
    if (*known != project)
      throw Failure("conflict",
                    "A conversation belongs to its original project");
    return;
  }
  auto q =
      db.prepare("INSERT OR IGNORE INTO conversation_locations VALUES(?,?)");
  q.bind(1, id).bind(2, project).run();
}

auto ConversationStore::ConversationDatabase(const std::string& id)
    -> sql::Db& {
  auto* store = FindConversationDatabase(id);
  if (!store) throw Failure("not_found", "Conversation not found");
  return *store;
}

auto ConversationStore::GetConversation(const std::string& id) -> Conversation {
  auto q = ConversationDatabase(id).prepare(
      "SELECT id,project,role,title,messages,updated FROM "
      "conversations WHERE id=?");
  q.bind(1, id);
  if (!q.step()) throw Failure("not_found", "Conversation not found");
  return {q.text(0), q.text(1), q.text(2), q.text(3), q.text(4), q.i64(5)};
}

auto ConversationStore::OwnConversation(const std::string& id, bool human)
    -> void {
  if (human) return;
  auto q = ConversationDatabase(id).prepare(
      "SELECT owner FROM conversations WHERE id=?");
  q.bind(1, id);
  if (!q.step() || q.text(0) != "external")
    throw Failure("permission_denied", "This conversation belongs to the user");
}

auto ConversationStore::Resources(const std::string& id)
    -> std::vector<std::string> {
  auto q = ConversationDatabase(id).prepare(
      "SELECT project FROM conversation_resources WHERE conversation=?");
  q.bind(1, id);
  std::vector<std::string> out;
  while (q.step()) out.push_back(q.text(0));
  return out;
}

auto ConversationStore::RememberResource(const std::string& id,
                                         const std::string& project) -> void {
  if (project.empty()) return;
  auto q = ConversationDatabase(id).prepare(
      "INSERT OR IGNORE INTO conversation_resources VALUES(?,?)");
  q.bind(1, id).bind(2, project).run();
}

auto ConversationStore::AuthorizeHistory(const std::string& id) -> void {
  for (const auto& resource : Resources(id))
    if (!has_grant(resource, Tsugumi::Service::Access::Read))
      throw Failure("permission_denied",
                    "Historical content belongs to a resource whose "
                    "permission was revoked");
}

auto ConversationStore::SaveMessages(const std::string& id,
                                     const std::vector<Message>& messages,
                                     SaveMode mode) -> void {
  std::lock_guard lock(gate.mutex);
  auto& database = ConversationDatabase(id);
  std::optional<sql::Transaction> transaction;
  if (sqlite3_get_autocommit(database.get())) transaction.emplace(database);
  const auto previous = ParseMessages(GetConversation(id).messages);
  if (auto found = presentation.find(id); found != presentation.end())
    found->second.dirty = true;
  const auto serialized = scrub(SerializeMessages(messages));
  // Compare the same persisted representation on both sides. Scrubbing also
  // orders JSON objects in providerContent; that is not a history replacement.
  // Preparing a send expands the provisional request in place. Only runtime
  // replacements remove conversation history that belongs in the archive.
  const auto removed =
      mode == SaveMode::PrepareSend
          ? std::vector<Message>{}
          : RemovedMessages(previous, ParseMessages(serialized));
  if (!removed.empty()) {
    auto archive = database.prepare("INSERT INTO archives VALUES(?,?)");
    archive.bind(1, id).bind(2, scrub(SerializeMessages(removed))).run();
  }
  for (const auto& resource : agent::Resources(messages))
    RememberResource(id, resource);
  auto q = ConversationDatabase(id).prepare(
      "UPDATE conversations SET messages=?,updated=? WHERE id=?");
  q.bind(1, serialized).bind(2, Now()).bind(3, id).run();
  if (transaction) transaction->commit();
}

auto ConversationStore::BuildConversation(const Conversation& c,
                                          std::string& streaming_id)
    -> A::ConversationT {
  std::vector<Message> messages;
  std::string summary;
  for (auto& message : ParseMessages(c.messages)) {
    if (message.context_summary) {
      for (const auto& block : message.content)
        if (block.type == "text") summary += block.text;
    } else {
      messages.push_back(std::move(message));
    }
  }
  std::vector<Message> archive;
  auto archived = ConversationDatabase(c.id).prepare(
      "SELECT messages FROM archives WHERE conversation=? ORDER BY rowid");
  archived.bind(1, c.id);
  while (archived.step())
    for (auto& message : ParseMessages(archived.text(0)))
      if (!message.context_summary) archive.push_back(std::move(message));
  auto stats = ConversationDatabase(c.id).prepare(
      "SELECT json FROM usage WHERE conversation=?");
  stats.bind(1, c.id);
  const auto usage_json =
      stats.step() ? stats.text(0) : R"({"inputTokens":0,"outputTokens":0})";
  std::string streaming;
  std::map<std::pair<std::string, uint32_t>, std::string> helper_streams;
  auto events = ConversationDatabase(c.id).prepare(
      "SELECT json FROM events WHERE conversation=? AND seq>coalesce("
      "(SELECT max(seq) FROM events WHERE conversation=? AND "
      "json_extract(json,'$.kind') IN ('checkpoint','finished')),0) ORDER BY "
      "seq");
  events.bind(1, c.id).bind(2, c.id);
  while (events.step()) {
    auto event = json::Object(events.text(0));
    auto* tail = &streaming;
    if (json::Text(event, "kind") == "helper") {
      tail = &helper_streams[{json::Text(event, "id"),
                              json::UInt(json::Get(event, "index"))}];
      event = json::Object(json::Get(event, "event", "{}"));
    }
    const auto kind = json::Text(event, "kind");
    if (kind == "text")
      *tail += json::Text(event, "text");
    else if (kind == "checkpoint" || kind == "discardText" ||
             kind == "finished")
      tail->clear();
  }
  std::vector<std::string> helpers;
  auto helper = ConversationDatabase(c.id).prepare(
      "SELECT parent_call,task,messages,result,state,spec,anchor,archive FROM "
      "helpers WHERE conversation=? ORDER BY rowid");
  helper.bind(1, c.id);
  while (helper.step()) {
    helpers.push_back(json::Encode(json::Members{
        {"call_id", json::Quote(helper.text(0))},
        {"task", std::to_string(helper.i64(1))},
        {"messages", helper.text(2)},
        {"archive", helper.text(7)},
        {"result", helper.text(3).empty() ? "null" : helper.text(3)},
        {"state", json::Quote(helper.text(4))},
        {"spec", helper.text(5)},
        {"anchor", std::to_string(helper.i64(6))},
        {"streaming",
         json::Quote(helper_streams[{
             helper.text(0), static_cast<uint32_t>(helper.i64(1))}])}}));
  }
  std::vector<std::string> tools;
  auto q = ConversationDatabase(c.id).prepare(
      "SELECT id,result,error,state FROM receipts WHERE "
      "conversation=? ORDER BY rowid");
  q.bind(1, c.id);
  while (q.step())
    tools.push_back(
        json::Encode(json::Members{{"id", json::Quote(q.text(0))},
                                   {"name", json::Quote("core")},
                                   {"arguments", json::Quote("{}")},
                                   {"result", json::Quote(q.text(1))},
                                   {"isError", q.i64(2) ? "true" : "false"},
                                   {"state", json::Quote(q.text(3))}}));
  auto approvals = db.prepare(
      "SELECT call,reason,state FROM permission_requests WHERE conversation=? "
      "AND access=? ORDER BY rowid");
  approvals.bind(1, c.id).bind(
      2, static_cast<int>(Tsugumi::Service::Access::CardPermissions));
  while (approvals.step()) {
    auto receipt = ConversationDatabase(c.id).prepare(
        "SELECT result,error FROM receipts WHERE conversation=? AND id=?");
    receipt.bind(1, c.id).bind(2, approvals.text(0));
    const bool recorded = receipt.step();
    const auto output = recorded ? receipt.text(0) : "";
    const bool error = recorded && receipt.i64(1);
    const auto outcome =
        approvals.text(2) == "denied"    ? "Declined; nothing changed"
        : approvals.text(2) == "pending" ? "Awaiting approval"
        : approvals.text(2) == "stale"
            ? "Targets changed; nothing executed. A new approval is required"
        : output.empty() ? "Approved; execution outcome not yet recorded"
                         : "Approved; outcome: " + output;
    tools.push_back(json::Encode(json::Members{
        {"id", json::Quote("approval:" + approvals.text(0))},
        {"name", json::Quote("operation_approval")},
        {"arguments", json::Quote("{}")},
        {"result", json::Quote(scrub(approvals.text(1) + "\n" + outcome))},
        {"isError",
         approvals.text(2) == "denied" || approvals.text(2) == "stale" || error
             ? "true"
             : "false"},
        {"state", json::Quote(approvals.text(2) == "pending" ? "pending"
                                                             : "completed")}}));
  }
  if (!streaming.empty())
    streaming_id =
        ":" + std::to_string(archive.size() + messages.size()) + ":0";
  return wire::Conversation(json::Members{
      {"id", json::Quote(c.id)},
      {"title", json::Quote(c.title)},
      {"updatedAt", std::to_string(c.updated)},
      {"messages", scrub(SerializeMessages(messages))},
      {"archive", SerializeMessages(archive)},
      {"summary", summary.empty() ? "null" : json::Quote(summary)},
      {"tools", json::Encode(tools)},
      {"helpers", json::Encode(helpers)},
      {"streaming", json::Quote(streaming)},
      {"usage", usage_json}});
}

auto ConversationStore::PresentationState::MarkChanged(size_t index) -> void {
  changes.erase({row_sequences[index], index});
  changes.emplace(seq, index);
  row_sequences[index] = seq;
}

auto ConversationStore::RebuildPresentation(A::ConversationT& snapshot,
                                            const std::string& streaming_id,
                                            PresentationState& state,
                                            bool full_snapshot) -> void {
  state.streaming_row.reset();
  const auto old_count = state.rows.size();
  const auto count = snapshot.rows.size();
  if (old_count != count) {
    state.resized_at = state.seq;
    if (full_snapshot) state.snapshot_resize = true;
  }
  for (size_t i = count; i < old_count; ++i) {
    state.changes.erase({state.row_sequences[i], i});
    state.snapshot_changes.erase(i);
  }
  state.row_sequences.resize(count);
  for (size_t i = 0; i < count; ++i) {
    if (snapshot.rows[i]->id == streaming_id) state.streaming_row = i;
    if (i >= old_count || *state.rows[i] != *snapshot.rows[i]) {
      if (state.seq > state.baseline) state.MarkChanged(i);
      if (full_snapshot) state.snapshot_changes.insert(i);
    }
  }
  state.rows = std::move(snapshot.rows);
  state.dirty = false;
}

auto ConversationStore::ConversationData(const Conversation& c)
    -> A::ConversationT {
  std::string streaming_id;
  auto snapshot = BuildConversation(c, streaming_id);
  auto [it, inserted] = presentation.try_emplace(c.id);
  auto& state = it->second;
  if (inserted) {
    auto latest = ConversationDatabase(c.id).prepare(
        "SELECT coalesce(max(seq),0) FROM events WHERE conversation=?");
    latest.bind(1, c.id).step();
    state.seq = state.baseline = latest.u64(0);
  }
  RebuildPresentation(snapshot, streaming_id, state, !inserted);
  snapshot.rows.reserve(state.rows.size());
  for (const auto& row : state.rows)
    snapshot.rows.push_back(std::make_unique<A::ChatRowT>(*row));
  return snapshot;
}

auto ConversationStore::Create(const A::ConversationCreateRequest& request,
                               bool human) -> Conversation {
  const auto* p = &request;
  auto id = Text(p->conversation_id());
  if (id.empty()) {
    do {
      id = host::AllocateId(db, "conversation");
    } while (FindConversationDatabase(id));
  }
  if (!crypto::is_operation_id(id))
    throw Failure("invalid_request", "Conversation ID must be a safe filename");
  const auto project = Text(p->project_id());
  if (!project.empty()) registry.Lookup(project);
  const auto role = Text(p->role()) == "guide" ? "guide" : "worker";
  if (const auto known = ConversationProject(id); known && *known != project)
    throw Failure("conflict", "A conversation belongs to its original project");
  if (FindConversationDatabase(id, project)) {
    OwnConversation(id, human);
    if (!human) AuthorizeHistory(id);
    const auto current = GetConversation(id);
    if (current.project != project)
      throw Failure("conflict",
                    "A conversation belongs to its original project");
    return current;
  }
  auto* store = ProjectDatabase(project, true);
  auto q =
      store->prepare("INSERT INTO conversations VALUES(?,?,?,'','[]',?,?)");
  q.bind(1, id)
      .bind(2, project)
      .bind(3, role)
      .bind(4, Now())
      .bind(5, human ? "user" : "external")
      .run();
  RememberConversation(id, project);
  return GetConversation(id);
}

auto ConversationStore::List(const A::ConversationListRequest& request,
                             bool human) -> A::ConversationListT {
  const auto project = Text(request.project_id());
  A::ConversationListT result;
  for (auto* store : Databases(project)) {
    auto q = store->prepare(
        "SELECT id,project,title,updated FROM conversations "
        "ORDER BY updated DESC LIMIT 500");
    while (q.step()) {
      RememberConversation(q.text(0), q.text(1));
      if (!human) {
        try {
          OwnConversation(q.text(0), false);
          AuthorizeHistory(q.text(0));
        } catch (const Failure&) {
          continue;
        }
      }
      auto row = std::make_unique<A::ConversationEntryT>();
      row->id = q.text(0);
      row->project_id = q.text(1);
      row->title = q.text(2);
      row->updated_at = q.i64(3);
      row->busy = is_running(q.text(0));
      result.conversations.push_back(std::move(row));
    }
  }
  std::stable_sort(result.conversations.begin(), result.conversations.end(),
                   [](const auto& a, const auto& b) {
                     return a->updated_at > b->updated_at;
                   });
  if (result.conversations.size() > 500) result.conversations.resize(500);
  return result;
}

auto ConversationStore::Delete(const std::string& id) -> void {
  if (is_running(id))
    throw Failure("busy", "Cancel the conversation before deleting it");
  auto& store = ConversationDatabase(id);
  // Remove conversation prompts; project grants outlive conversations.
  {
    sql::Transaction tx(db);
    for (const auto* table :
         {"providers", "skill_catalogs", "permission_requests"}) {
      auto q = db.prepare(std::string("DELETE FROM ") + table +
                          " WHERE conversation=?");
      q.bind(1, id).run();
    }
    tx.commit();
  }
  {
    sql::Transaction tx(store);
    for (const auto* table :
         {"events", "receipts", "conversation_turns", "model_bindings",
          "archives", "usage", "conversation_resources", "helpers",
          "helper_delegations", "call_counters"}) {
      auto q = store.prepare(std::string("DELETE FROM ") + table +
                             " WHERE conversation=?");
      q.bind(1, id).run();
    }
    auto q = store.prepare("DELETE FROM conversations WHERE id=?");
    q.bind(1, id).run();
    tx.commit();
  }
  auto location =
      db.prepare("DELETE FROM conversation_locations WHERE conversation=?");
  location.bind(1, id).run();
  presentation.erase(id);
}

auto ConversationStore::Events(const A::EventsRequest& request, bool human)
    -> A::EventsPageT {
  const auto* p = &request;
  const auto id = p->conversation_id()->str();
  OwnConversation(id, human);
  if (!human) AuthorizeHistory(id);
  auto q = ConversationDatabase(id).prepare(
      "SELECT seq,json FROM events WHERE conversation=? "
      "AND seq>? ORDER BY seq LIMIT ?");
  q.bind(1, id)
      .bind(2, p->after_seq())
      .bind(3, static_cast<int>(std::clamp(p->limit(), 1u, 1000u)));
  A::EventsPageT result;
  result.next_seq = p->after_seq();
  result.busy = is_running(id);
  while (q.step()) {
    result.next_seq = q.u64(0);
    auto event = std::make_unique<A::SequencedEventT>();
    event->seq = result.next_seq;
    event->event = std::make_unique<A::EventT>(wire::Event(q.text(1)));
    result.events.push_back(std::move(event));
  }
  if (result.events.empty()) return result;

  auto [it, inserted] = presentation.try_emplace(id);
  auto& state = it->second;
  if (inserted || state.dirty) {
    if (inserted) {
      auto latest = ConversationDatabase(id).prepare(
          "SELECT coalesce(max(seq),0) FROM events WHERE conversation=?");
      latest.bind(1, id).step();
      state.seq = state.baseline = latest.u64(0);
    }
    std::string streaming_id;
    auto snapshot = BuildConversation(GetConversation(id), streaming_id);
    RebuildPresentation(snapshot, streaming_id, state, false);
  }

  const auto after = p->after_seq();
  const bool reset = after < state.baseline;
  auto change = state.changes.upper_bound({after, SIZE_MAX});
  if (!reset && change == state.changes.end() && state.resized_at <= after)
    return result;
  auto update = std::make_unique<A::PresentationUpdateT>();
  update->row_count = static_cast<uint32_t>(state.rows.size());
  update->reset = reset;
  update->seq = state.seq;
  const auto append = [&](size_t index) {
    auto row = std::make_unique<A::ChatRowUpdateT>();
    row->index = static_cast<uint32_t>(index);
    row->row = std::make_unique<A::ChatRowT>(*state.rows[index]);
    update->rows.push_back(std::move(row));
  };
  if (reset) {
    for (size_t i = 0; i < state.rows.size(); ++i) append(i);
  } else {
    for (; change != state.changes.end(); ++change) append(change->second);
    std::ranges::sort(update->rows, {}, &A::ChatRowUpdateT::index);
  }
  result.presentation = std::move(update);
  return result;
}

auto ConversationStore::RecordEvent(const std::string& id,
                                    const std::string& raw) -> void {
  auto& database = id.empty() ? db : ConversationDatabase(id);
  auto q =
      database.prepare("INSERT INTO events(conversation,json) VALUES(?,?)");
  q.bind(1, id).bind(2, raw).run();
  const auto found = presentation.find(id);
  if (found == presentation.end()) return;
  auto& state = found->second;
  state.seq = static_cast<uint64_t>(sqlite3_last_insert_rowid(database.get()));
  for (const auto index : state.snapshot_changes) state.MarkChanged(index);
  state.snapshot_changes.clear();
  if (std::exchange(state.snapshot_resize, false)) state.resized_at = state.seq;
  if (state.dirty) return;
  const auto event = json::Object(raw);
  const auto kind = json::Text(event, "kind");
  if (kind == "text") {
    const auto text = json::Text(event, "text");
    if (text.empty()) return;
    if (state.streaming_row) {
      state.rows[*state.streaming_row]->text += text;
      state.MarkChanged(*state.streaming_row);
    } else {
      state.dirty = true;
    }
  } else if (kind != "toolInput") {
    state.dirty = true;
  }
}
}  // namespace tsugumi::agent
