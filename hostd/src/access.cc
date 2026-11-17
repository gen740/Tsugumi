#include "host/access.hh"

#include <set>

#include "tsugumi/util/json_object.hh"
#include "agent/presentation.hh"
#include "agent/store.hh"
#include "agent/wire.hh"
#include "host/failure.hh"
#include "host/projects.hh"
#include "tsugumi/util/crypto.hh"

namespace tsugumi::host {
namespace A = Tsugumi::Agent;
namespace S = Tsugumi::Service;
namespace json = ::tsugumi::json;
namespace wire = agent::wire;
using agent::Cancelled;
using agent::Message;
namespace {
auto Text(const flatbuffers::String* value) -> std::string {
  return value ? value->str() : "";
}
auto Human(bool human) -> void {
  if (!human)
    throw Failure("permission_denied",
                  "This operation requires the authenticated user");
}

}  // namespace
Access::Access(StateGate& gate, sql::Db& db, ProjectRegistry& registry,
               agent::ConversationStore& conversations, bool& stopping,
               Dependencies dependencies)
    : mutex(gate.mutex),
      db(db),
      registry(registry),
      conversations(&conversations),
      stopping(stopping),
      dependencies(std::move(dependencies)) {}

auto Access::HasGrant(const std::string& project,
                      const std::string& conversation, Tsugumi::Service::Access access)
    -> bool {
  if (project.empty()) return true;
  if (project.starts_with("helper-scope:")) {
    auto approval = db.prepare(
        "SELECT 1 FROM access_requests WHERE id=? AND "
        "conversation=? AND access=? AND state='allowed'");
    approval.bind(1, project.substr(13))
        .bind(2, conversation)
        .bind(3, static_cast<int>(Tsugumi::Service::Access::HelperScope));
    return access == Tsugumi::Service::Access::Read && approval.step();
  }
  auto q = db.prepare(
      "SELECT 1 FROM grants WHERE project=? AND (conversation='' "
      "OR conversation=?) AND access=? AND active=1 LIMIT 1");
  q.bind(1, project).bind(2, conversation).bind(3, static_cast<int>(access));
  return q.step();
}

auto Access::AccessDescription(const std::string& resource, Tsugumi::Service::Access access,
                               Tsugumi::Service::GrantScope scope, const std::string& reason,
                               bool needed) -> std::string {
  if (reason.starts_with("operation_approval\n") ||
      reason.starts_with("helper_scope\n"))
    return "project_access " + reason;
  auto q = db.prepare(
      "SELECT title,coalesce((SELECT path FROM project_paths WHERE "
      "id=projects.id),'') "
      "FROM projects WHERE id=? UNION ALL SELECT title,path FROM folders WHERE "
      "id=?");
  q.bind(1, resource).bind(2, resource);
  const bool found = q.step();
  auto title = found ? q.text(0) : std::string{};
  auto path = found ? q.text(1) : std::string{};
  if (!found) {
    auto stored = db.prepare("SELECT path FROM project_paths WHERE id=?");
    stored.bind(1, resource);
    if (stored.step()) path = stored.text(0);
  }
  if (title.empty() && !path.empty()) title = host::ProjectDisplayName(path);
  return "project_access " +
         json::Encode(
             json::Members{{"title", json::Quote(title)},
                           {"path", json::Quote(path)},
                           {"access", json::Quote(Tsugumi::Service::EnumNameAccess(access))},
                           {"scope", json::Quote(Tsugumi::Service::EnumNameGrantScope(scope))},
                           {"reason", json::Quote(reason)},
                           {"needed_now", needed ? "true" : "false"}});
}

auto Access::AccessDescriptions(const std::string& id,
                                const std::string& project, Tsugumi::Service::Access needed,
                                Tsugumi::Service::GrantScope scope, const std::string& reason)
    -> std::vector<std::string> {
  std::vector<std::string> asked{
      AccessDescription(project, needed, scope, reason)};
  auto items = db.prepare(
      "SELECT access FROM access_prompt_items WHERE request=? ORDER BY access");
  items.bind(1, id);
  while (items.step()) {
    const auto access = static_cast<Tsugumi::Service::Access>(items.i64(0));
    if (access != needed)
      asked.push_back(AccessDescription(project, access, scope, reason, false));
  }
  return asked;
}

auto Access::RequestAccess(const std::string& project,
                           const std::string& conversation, Tsugumi::Service::Access access,
                           Tsugumi::Service::GrantScope scope, const std::string& reason,
                           bool per_call, bool combine) -> std::string {
  if (!per_call && (access == Tsugumi::Service::Access::HelperScope ||
                    reason.find("operation_approval\n") != std::string::npos ||
                    reason.find("helper_scope\n") != std::string::npos))
    throw Failure("invalid_request",
                  "The operation approval marker is reserved for "
                  "core-generated requests");
  auto folder = db.prepare("SELECT project FROM folders WHERE id=?");
  folder.bind(1, project);
  registry.Lookup(folder.step() ? folder.text(0) : project);
  auto old = db.prepare(
      "SELECT id FROM access_requests WHERE project=? AND conversation=? AND "
      "access=? AND scope=? AND (state='pending' OR (access=? AND "
      "state='allowed')) AND "
      "((?=1 AND reason=?) OR (?=0 AND id NOT IN (SELECT request FROM "
      "operation_approvals)))");
  old.bind(1, project)
      .bind(2, conversation)
      .bind(3, static_cast<int>(access))
      .bind(4, static_cast<int>(scope))
      .bind(5, static_cast<int>(Tsugumi::Service::Access::HelperScope))
      .bind(6, per_call ? 1 : 0)
      .bind(7, reason)
      .bind(8, per_call ? 1 : 0);
  if (old.step()) return old.text(0);
  sql::Transaction tx(db);
  const auto id = crypto::uuid4();
  auto q =
      db.prepare("INSERT INTO access_requests VALUES(?,?,?,?,?,?, 'pending')");
  q.bind(1, id)
      .bind(2, project)
      .bind(3, conversation)
      .bind(4, static_cast<int>(access))
      .bind(5, static_cast<int>(scope))
      .bind(6, reason)
      .run();
  // Only ordinary project access is bundled. Folder access, shell access and
  // per-operation approvals retain their own, narrower questions.
  if (combine && access <= Tsugumi::Service::Access::Run && !conversation.empty() &&
      conversations->GetConversation(conversation).project == project) {
    for (const auto candidate :
         {Tsugumi::Service::Access::Read, Tsugumi::Service::Access::Write, Tsugumi::Service::Access::Run}) {
      auto asked = db.prepare(
          "SELECT 1 FROM access_requests r JOIN access_prompt_items i ON "
          "i.request=r.id "
          "WHERE r.project=? AND r.conversation=? AND i.access=? "
          "UNION ALL SELECT 1 FROM access_requests WHERE project=? AND "
          "conversation=? AND access=?");
      asked.bind(1, project)
          .bind(2, conversation)
          .bind(3, static_cast<int>(candidate))
          .bind(4, project)
          .bind(5, conversation)
          .bind(6, static_cast<int>(candidate));
      if (candidate != access &&
          (HasGrant(project, conversation, candidate) || asked.step()))
        continue;
      auto item = db.prepare("INSERT INTO access_prompt_items VALUES(?,?)");
      item.bind(1, id).bind(2, static_cast<int>(candidate)).run();
    }
  }
  std::vector<std::string> asked;
  for (const auto& description :
       AccessDescriptions(id, project, access, scope, reason))
    asked.push_back(json::Quote(description));
  tx.commit();
  dependencies.event(conversation, json::Encode(json::Members{
                                       {"kind", json::Quote("permission")},
                                       {"id", json::Quote(id)},
                                       {"asked", json::Encode(asked)}}));
  return id;
}

auto Access::Require(const std::string& project,
                     const std::string& conversation, Tsugumi::Service::Access access,
                     bool wait, std::stop_token stop, const std::string& reason,
                     const std::string& authorization) -> void {
  std::unique_lock lock(mutex);
  if (!authorization.empty()) {
    if (!HasGrant(authorization, conversation, Tsugumi::Service::Access::Read))
      throw Failure("permission_denied",
                    "The helper scope approval is no longer valid");
    return;
  }
  if (HasGrant(project, conversation, access)) return;
  const auto id =
      RequestAccess(project, conversation, access, Tsugumi::Service::GrantScope::Conversation,
                    reason, false, true);
  if (!wait)
    throw Failure("grant_required", "Explicit user permission is required", id);
  while (!HasGrant(project, conversation, access)) {
    if (stop.stop_requested() || stopping) throw Cancelled();
    auto q = db.prepare("SELECT state FROM access_requests WHERE id=?");
    q.bind(1, id);
    if (!q.step() || q.text(0) == "denied")
      throw Failure("permission_denied", "The user declined access");
    changed.wait_for(lock, std::chrono::milliseconds(100));
  }
}

auto Access::AuthorizeContext(const std::string& id,
                              const std::vector<Message>& messages) -> void {
  std::lock_guard lock(mutex);
  for (const auto& message : messages)
    for (const auto& block : message.content)
      for (const auto& resource : block.resource_ids)
        if (!HasGrant(resource, id, Tsugumi::Service::Access::Read))
          throw Failure("permission_denied",
                        "The conversation contains content whose permission "
                        "was revoked; clear it before continuing");
}

auto Access::Handle(const RequestIn& request, bool human) -> std::string {
  const auto* r = A::GetMessage(request.frame->data())->payload_as_Request();
  switch (r->value_type()) {
    case A::RequestPayload::AccessRequest: {
      auto p = r->value_as_AccessRequest();
      if (p->access() > Tsugumi::Service::Access::MAX || p->scope() > Tsugumi::Service::GrantScope::MAX)
        throw Failure("invalid_request", "Invalid permission scope");
      const auto conversation = Text(p->conversation_id());
      if (p->scope() == Tsugumi::Service::GrantScope::Conversation && conversation.empty())
        throw Failure("invalid_request",
                      "A conversation-scoped grant needs a conversation");
      if (!conversation.empty()) conversations->GetConversation(conversation);
      if (!conversation.empty())
        conversations->OwnConversation(conversation, human);
      const auto id = RequestAccess(p->project_id()->str(), conversation,
                                    p->access(), p->scope(), Text(p->reason()));
      A::AccessResultT result;
      result.id = id;
      return wire::AgentReply(request.request_id, std::move(result));
    }
    case A::RequestPayload::AccessResolveRequest: {
      Human(human);
      auto p = r->value_as_AccessResolveRequest();
      auto q = db.prepare(
          "SELECT project,conversation,access,scope,state FROM "
          "access_requests WHERE id=?");
      q.bind(1, p->request_id()->str());
      if (!q.step()) throw Failure("not_found", "Access request not found");
      if (q.text(4) != "pending")
        throw Failure("conflict", "Access request was already answered");
      sql::Transaction tx(db);
      auto operation =
          db.prepare("SELECT 1 FROM operation_approvals WHERE request=?");
      operation.bind(1, p->request_id()->str());
      const bool per_call =
          operation.step() ||
          static_cast<Tsugumi::Service::Access>(q.i64(2)) == Tsugumi::Service::Access::HelperScope;
      if (p->allow() && !per_call &&
          static_cast<Tsugumi::Service::Access>(q.i64(2)) != Tsugumi::Service::Access::HelperScope) {
        auto grant = db.prepare("INSERT INTO grants VALUES(?,?,?,?,1)");
        grant.bind(1, p->request_id()->str())
            .bind(2, q.text(0))
            .bind(3, q.i64(3) == 1 ? "" : q.text(1))
            .bind(4, q.i64(2))
            .run();
      }
      std::set<Tsugumi::Service::Access> offered;
      auto items =
          db.prepare("SELECT access FROM access_prompt_items WHERE request=?");
      items.bind(1, p->request_id()->str());
      while (items.step()) offered.insert(static_cast<Tsugumi::Service::Access>(items.i64(0)));
      if (p->also_allow()) {
        for (const auto access : *p->also_allow()) {
          if (per_call || access == static_cast<Tsugumi::Service::Access>(q.i64(2)) ||
              !offered.contains(access))
            throw Failure(
                "invalid_request",
                "Only other accesses listed in this prompt can be allowed");
        }
        std::set<Tsugumi::Service::Access> allowed(p->also_allow()->begin(),
                                    p->also_allow()->end());
        for (const auto access : allowed) {
          auto grant = db.prepare("INSERT INTO grants VALUES(?,?,?,?,1)");
          grant.bind(1, crypto::uuid4())
              .bind(2, q.text(0))
              .bind(3, q.i64(3) == 1 ? "" : q.text(1))
              .bind(4, static_cast<int>(access))
              .run();
        }
      }
      auto update = db.prepare("UPDATE access_requests SET state=? WHERE id=?");
      update.bind(1, p->allow() ? "allowed" : "denied")
          .bind(2, p->request_id()->str())
          .run();
      tx.commit();
      dependencies.event(
          q.text(1),
          json::Encode(json::Members{
              {"kind", json::Quote("permissionAnswered")},
              {"id", json::Quote(p->request_id()->str())},
              {"ok", p->allow() ? "true" : "false"},
              {"text",
               json::Quote(p->allow() ? "Approved this request"
                                      : "Declined; nothing changed")}}));
      changed.notify_all();
      return wire::AgentReply(request.request_id, S::AcknowledgementT{});
    }
    case A::RequestPayload::AccessRevokeRequest: {
      Human(human);
      const auto grant_id =
          r->value_as_AccessRevokeRequest()->grant_id()->str();
      auto grant = db.prepare(
          "SELECT project,conversation,access FROM grants WHERE id=? AND "
          "active=1");
      grant.bind(1, grant_id);
      if (grant.step()) {
        const auto project = grant.text(0), conversation = grant.text(1);
        const auto access = static_cast<Tsugumi::Service::Access>(grant.i64(2));
        if (access == Tsugumi::Service::Access::Shell || access == Tsugumi::Service::Access::Read ||
            access == Tsugumi::Service::Access::Write || access == Tsugumi::Service::Access::Run) {
          dependencies.cancel(project, conversation);
        }
      }
      auto q = db.prepare("UPDATE grants SET active=0 WHERE id=?");
      q.bind(1, grant_id).run();
      changed.notify_all();
      return wire::AgentReply(request.request_id, S::AcknowledgementT{});
    }
    case A::RequestPayload::AccessListRequest: {
      const auto id = Text(r->value_as_AccessListRequest()->conversation_id());
      if (!human && id.empty())
        throw Failure("permission_denied",
                      "External clients must name their conversation");
      if (!id.empty()) {
        conversations->OwnConversation(id, human);
        if (!human) conversations->AuthorizeHistory(id);
      }
      A::AccessListT result;
      auto q = db.prepare(
          "SELECT id,project,conversation,access,scope,reason,state "
          "FROM access_requests WHERE (?='' OR conversation=?) "
          "ORDER BY rowid DESC LIMIT 500");
      q.bind(1, id).bind(2, id);
      while (q.step()) {
        auto entry = std::make_unique<A::AccessEntryT>();
        entry->id = q.text(0);
        entry->project_id = q.text(1);
        entry->conversation_id = q.text(2);
        entry->access = static_cast<Tsugumi::Service::Access>(q.i64(3));
        entry->scope = static_cast<Tsugumi::Service::GrantScope>(q.i64(4));
        entry->reason = q.text(5);
        entry->state = q.text(6);
        entry->asked =
            AccessDescriptions(entry->id, entry->project_id, entry->access,
                               entry->scope, entry->reason);
        entry->groups = agent::AccessGroups(entry->asked);
        result.requests.push_back(std::move(entry));
      }
      auto g = db.prepare(
          "SELECT id,project,conversation,access,active FROM grants "
          "WHERE (?='' OR conversation='' OR conversation=?)");
      g.bind(1, id).bind(2, id);
      while (g.step()) {
        auto grant = std::make_unique<A::GrantT>();
        grant->id = g.text(0);
        grant->project_id = g.text(1);
        grant->conversation_id = g.text(2);
        grant->access = static_cast<Tsugumi::Service::Access>(g.i64(3));
        grant->active = g.i64(4) != 0;
        result.grants.push_back(std::move(grant));
      }
      return wire::AgentReply(request.request_id, std::move(result));
    }
    default:
      throw Failure("invalid_request", "Unsupported access request");
  }
}
}  // namespace tsugumi::host
