#include "agent/store.hh"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <utility>

#include "agent/json.hh"
#include "agent/wire.hh"
#include "host/failure.hh"
#include "util/crypto.hh"

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
auto Excerpt(std::string_view text, size_t position = 0, size_t limit = 240)
    -> std::string {
  size_t begin = position > limit / 3 ? position - limit / 3 : 0;
  while (begin && (static_cast<unsigned char>(text[begin]) & 0xc0) == 0x80)
    --begin;
  size_t end = std::min(text.size(), begin + limit);
  while (end < text.size() && end > begin &&
         (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80)
    --end;
  return (begin ? "…" : "") + std::string(text.substr(begin, end - begin)) +
         (end < text.size() ? "…" : "");
}

auto FoldAscii(std::string text) -> std::string {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return text;
}

// Conversation records contain no provisioning, host capabilities or settings.
auto CreateConversationTables(sql::Db& db) -> void {
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
      "CREATE TABLE IF NOT EXISTS turns(conversation TEXT NOT NULL, id TEXT "
      "NOT NULL, digest TEXT NOT NULL, state TEXT NOT NULL, action INTEGER "
      "NOT NULL, operation TEXT NOT NULL, PRIMARY "
      "KEY(conversation,id));"
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
      "CREATE TABLE IF NOT EXISTS helpers(conversation TEXT NOT NULL, "
      "parent_call TEXT NOT NULL, task INTEGER NOT NULL, digest TEXT NOT "
      "NULL, messages TEXT NOT NULL, result TEXT NOT NULL, state TEXT "
      "NOT NULL, PRIMARY KEY(conversation,parent_call,task))");
  db.exec(
      "CREATE TABLE IF NOT EXISTS pending_sends(conversation TEXT "
      "PRIMARY KEY, turn TEXT NOT NULL, frame BLOB NOT NULL, "
      "message_index INTEGER NOT NULL)");
}

}  // namespace

struct ConversationStore::ProjectStore {
  std::filesystem::path path;
  fs::FileLock lock;
  sql::Db db;
  ProjectStore(std::filesystem::path p, fs::FileLock l);
};
ConversationStore::ProjectStore::ProjectStore(std::filesystem::path p,
                                              fs::FileLock l)
    : path(std::move(p)),
      lock(std::move(l)),
      db((path / "agent.sqlite").string()) {
  ::chmod((path / "agent.sqlite").c_str(), 0600);
  // agentd writes the original host folder, never the VM's virtiofs mount.
  // DELETE keeps a quiescent project copy self-contained (no WAL sidecar).
  db.exec("PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;");
  CreateConversationTables(db);
  db.exec("UPDATE turns SET state='interrupted' WHERE state='running'");
}

ConversationStore::ConversationStore(host::StateGate& g, sql::Db& home,
                                     host::ProjectRegistry& projects,
                                     Scrub redact, HasGrant grant,
                                     IsRunning busy)
    : gate(g),
      db(home),
      registry(projects),
      scrub(std::move(redact)),
      has_grant(std::move(grant)),
      is_running(std::move(busy)) {
  CreateConversationTables(db);
  db.exec("UPDATE turns SET state='interrupted' WHERE state='running'");
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
  auto lock = fs::FileLock::try_acquire(path / "agent.lock");
  if (!lock)
    throw Failure("busy",
                  "Another Agent host has this project's conversations open");
  auto store = std::make_unique<ProjectStore>(path, std::move(*lock));
  auto identity =
      store->db.prepare("SELECT 1 FROM conversations WHERE project<>? LIMIT 1");
  identity.bind(1, project);
  if (identity.step())
    throw Failure("conflict",
                  "Conversation records belong to another project identity");
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
  for (const auto& id : registry.Locations())
    if (auto* store = ProjectDatabase(id)) result.push_back(store);
  return result;
}

auto ConversationStore::FindConversationDatabase(const std::string& id)
    -> sql::Db* {
  auto contains = [&](sql::Db& store) {
    auto q = store.prepare("SELECT 1 FROM conversations WHERE id=?");
    q.bind(1, id);
    return q.step();
  };
  // Known conversations remain usable when an unrelated project is offline
  // or locked by another host. No conversation contents are cached here.
  sql::Db* result = contains(db) ? &db : nullptr;
  for (const auto& [_, store] : project_stores)
    if (contains(store->db)) {
      if (result)
        throw Failure("conflict",
                      "Conversation ID exists in multiple projects");
      result = &store->db;
    }
  if (result) return result;
  for (auto* store : Databases())
    if (contains(*store)) {
      if (result)
        throw Failure("conflict",
                      "Conversation ID exists in multiple projects");
      result = store;
    }
  return result;
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
    if (!has_grant(resource, id, Tsugumi::Service::Access::Read))
      throw Failure("permission_denied",
                    "Historical content belongs to a resource whose "
                    "permission was revoked");
}

auto ConversationStore::SaveMessages(const std::string& id,
                                     const std::vector<Message>& messages)
    -> void {
  std::lock_guard lock(gate.mutex);
  auto before = GetConversation(id);
  const auto previous = ParseMessages(before.messages);
  if (messages.size() < previous.size()) {
    size_t retained = 0;
    while (retained < messages.size() && retained < previous.size() &&
           SerializeMessages({messages[messages.size() - retained - 1]}) ==
               SerializeMessages({previous[previous.size() - retained - 1]}))
      ++retained;
    const std::vector<Message> removed(previous.begin(),
                                       previous.end() - retained);
    auto archive =
        ConversationDatabase(id).prepare("INSERT INTO archives VALUES(?,?)");
    archive.bind(1, id).bind(2, scrub(SerializeMessages(removed))).run();
  }
  for (const auto& message : messages)
    for (const auto& block : message.content)
      for (const auto& resource : block.resource_ids)
        RememberResource(id, resource);
  auto q = ConversationDatabase(id).prepare(
      "UPDATE conversations SET messages=?,updated=? WHERE id=?");
  q.bind(1, scrub(SerializeMessages(messages)))
      .bind(2, Now())
      .bind(3, id)
      .run();
}

auto ConversationStore::ConversationJson(const Conversation& c) -> std::string {
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
  std::vector<std::string> helpers;
  auto helper = ConversationDatabase(c.id).prepare(
      "SELECT parent_call,task,messages,result,state FROM helpers "
      "WHERE conversation=? ORDER BY rowid");
  helper.bind(1, c.id);
  while (helper.step())
    helpers.push_back(json::Encode(json::Members{
        {"call_id", json::Quote(helper.text(0))},
        {"task", std::to_string(helper.i64(1))},
        {"messages", helper.text(2)},
        {"result", helper.text(3).empty() ? "null" : helper.text(3)},
        {"state", json::Quote(helper.text(4))}}));
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
      "SELECT o.call,o.preview,a.state FROM operation_approvals o "
      "JOIN access_requests a ON a.id=o.request WHERE o.conversation=? ORDER "
      "BY o.rowid");
  approvals.bind(1, c.id);
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
  return json::Encode(json::Members{
      {"id", json::Quote(c.id)},
      {"title", json::Quote(c.title)},
      {"updatedAt", std::to_string(c.updated)},
      {"messages", scrub(SerializeMessages(messages))},
      {"archive", SerializeMessages(archive)},
      {"summary", summary.empty() ? "null" : json::Quote(summary)},
      {"tools", json::Encode(tools)},
      {"helpers", json::Encode(helpers)},
      {"usage", usage_json}});
}

auto ConversationStore::Create(const A::ConversationCreateRequest& request,
                               bool human) -> Conversation {
  const auto* p = &request;
  auto id = Text(p->conversation_id());
  if (id.empty()) id = crypto::uuid4();
  if (id.size() > 256)
    throw Failure("invalid_request", "Conversation ID is too long");
  const auto project = Text(p->project_id());
  if (!project.empty()) registry.Lookup(project);
  const auto role = Text(p->role()) == "guide" ? "guide" : "worker";
  if (FindConversationDatabase(id)) {
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
  // Revoke host capabilities first: interruption must never leave grants
  // for a deleted ID that a new conversation could reuse.
  {
    sql::Transaction tx(db);
    auto prompts = db.prepare(
        "DELETE FROM access_prompt_items WHERE request IN "
        "(SELECT id FROM access_requests WHERE conversation=?)");
    prompts.bind(1, id).run();
    for (const auto* table :
         {"providers", "skill_catalogs", "operation_approvals",
          "access_requests", "grants"}) {
      auto q = db.prepare(std::string("DELETE FROM ") + table +
                          " WHERE conversation=?");
      q.bind(1, id).run();
    }
    tx.commit();
  }
  {
    sql::Transaction tx(store);
    for (const auto* table :
         {"events", "receipts", "turns", "model_bindings", "archives", "usage",
          "conversation_resources", "helpers", "pending_sends"}) {
      auto q = store.prepare(std::string("DELETE FROM ") + table +
                             " WHERE conversation=?");
      q.bind(1, id).run();
    }
    auto q = store.prepare("DELETE FROM conversations WHERE id=?");
    q.bind(1, id).run();
    tx.commit();
  }
}

auto ConversationStore::Search(const A::ConversationSearchRequest& request,
                               bool human) -> A::SearchResultT {
  const auto* p = &request;
  A::SearchResultT result;
  for (auto* store : Databases(Text(p->project_id()))) {
    auto q = store->prepare(
        "SELECT id,project,messages,title,updated FROM "
        "conversations c WHERE "
        "(instr(lower(messages),lower(?))>0 OR EXISTS(SELECT "
        "1 FROM archives a WHERE a.conversation=c.id AND "
        "instr(lower(a.messages),lower(?))>0)) ORDER BY "
        "updated DESC LIMIT 100");
    q.bind(1, p->text()->str()).bind(2, p->text()->str());
    while (q.step()) {
      if (!human) {
        try {
          OwnConversation(q.text(0), false);
          AuthorizeHistory(q.text(0));
        } catch (const Failure&) {
          continue;
        }
      }
      std::string history;
      auto append = [&](const std::string& raw) {
        for (const auto& message : ParseMessages(raw)) {
          if (message.context_summary) continue;
          for (const auto& block : message.content)
            if (!block.text.empty()) history += block.text + '\n';
        }
      };
      auto archive = store->prepare(
          "SELECT messages FROM archives WHERE "
          "conversation=? ORDER BY rowid");
      archive.bind(1, q.text(0));
      while (archive.step()) append(archive.text(0));
      append(q.text(2));
      const auto found = FoldAscii(history).find(FoldAscii(p->text()->str()));
      if (found == std::string::npos) continue;
      auto row = std::make_unique<A::SearchMatchT>();
      row->session = q.text(0);
      row->title = q.text(3);
      row->snippet = Excerpt(history, found);
      row->updated_at = q.i64(4);
      result.matches.push_back(std::move(row));
    }
  }
  std::stable_sort(result.matches.begin(), result.matches.end(),
                   [](const auto& a, const auto& b) {
                     return a->updated_at > b->updated_at;
                   });
  if (result.matches.size() > 100) result.matches.resize(100);
  return result;
}

auto ConversationStore::Events(const A::EventsRequest& request, bool human)
    -> A::EventsPageT {
  const auto* p = &request;
  const auto id = p->conversation_id()->str();
  auto c = GetConversation(id);
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
  return result;
}

auto ConversationStore::RecordEvent(const std::string& id,
                                    const std::string& raw) -> void {
  auto q = (id.empty() ? db : ConversationDatabase(id))
               .prepare("INSERT INTO events(conversation,json) VALUES(?,?)");
  q.bind(1, id).bind(2, raw).run();
}
}  // namespace tsugumi::agent
