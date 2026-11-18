#include "host/access.hh"

#include <set>

#include "agent/context.hh"
#include "agent/presentation.hh"
#include "agent/store.hh"
#include "agent/wire.hh"
#include "host/failure.hh"
#include "host/projects.hh"
#include "host/wire_helpers.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/json_object.hh"

namespace tsugumi::host {
namespace A = Tsugumi::Agent;
namespace S = Tsugumi::Service;
namespace json = ::tsugumi::json;
namespace wire = agent::wire;
using agent::Cancelled;
using agent::Message;
namespace {}  // namespace
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
                      const std::string& conversation,
                      Tsugumi::Service::Access access) -> bool {
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
      "SELECT 1 FROM grants WHERE project=? AND access=? AND active=1 LIMIT 1");
  q.bind(1, project).bind(2, static_cast<int>(access));
  return q.step();
}

auto Access::AccessDescription(const std::string& resource,
                               Tsugumi::Service::Access access,
                               const std::string& reason, bool needed)
    -> std::string {
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
         json::Encode(json::Members{
             {"title", json::Quote(title)},
             {"path", json::Quote(path)},
             {"access", json::Quote(Tsugumi::Service::EnumNameAccess(access))},
             {"access_id", std::to_string(static_cast<unsigned>(access))},
             {"reason", json::Quote(reason)},
             {"needed_now", needed ? "true" : "false"}});
}

auto Access::AccessDescriptions(const std::string& id,
                                const std::string& project,
                                Tsugumi::Service::Access needed,
                                const std::string& reason, bool per_call)
    -> std::vector<std::string> {
  if (per_call)
    return {(needed == S::Access::HelperScope
                 ? "project_access helper_scope\n"
                 : "project_access operation_approval\n") +
            reason};
  std::vector<std::string> asked{AccessDescription(project, needed, reason)};
  auto items = db.prepare(
      "SELECT access FROM access_prompt_items WHERE request=? ORDER BY access");
  items.bind(1, id);
  while (items.step()) {
    const auto access = static_cast<Tsugumi::Service::Access>(items.i64(0));
    if (access != needed)
      asked.push_back(AccessDescription(project, access, reason, false));
  }
  return asked;
}

auto Access::RequestAccess(const std::string& project,
                           const std::string& conversation,
                           Tsugumi::Service::Access access,
                           const std::string& reason, bool per_call,
                           bool combine) -> std::string {
  if (!per_call && access == S::Access::HelperScope)
    throw Failure("invalid_request",
                  "Helper scope is a core-generated request");
  auto folder = db.prepare("SELECT project FROM folders WHERE id=?");
  folder.bind(1, project);
  registry.Lookup(folder.step() ? folder.text(0) : project);
  auto old = db.prepare(
      "SELECT id FROM access_requests WHERE project=? AND conversation=? AND "
      "access=? AND (state='pending' OR (access=? AND "
      "state='allowed')) AND "
      "((?=1 AND reason=?) OR (?=0 AND id NOT IN (SELECT request FROM "
      "operation_approvals)))");
  old.bind(1, project)
      .bind(2, conversation)
      .bind(3, static_cast<int>(access))
      .bind(4, static_cast<int>(Tsugumi::Service::Access::HelperScope))
      .bind(5, per_call ? 1 : 0)
      .bind(6, reason)
      .bind(7, per_call ? 1 : 0);
  if (old.step()) return old.text(0);
  sql::Transaction tx(db);
  const auto id = crypto::uuid4();
  auto q =
      db.prepare("INSERT INTO access_requests VALUES(?,?,?,?,?, 'pending')");
  q.bind(1, id)
      .bind(2, project)
      .bind(3, conversation)
      .bind(4, static_cast<int>(access))
      .bind(5, reason)
      .run();
  // Only ordinary project access is bundled. Folder access, shell access and
  // per-operation approvals retain their own, narrower questions.
  if (combine && access <= Tsugumi::Service::Access::Run &&
      !conversation.empty() &&
      conversations->GetConversation(conversation).project == project) {
    for (const auto candidate :
         {Tsugumi::Service::Access::Read, Tsugumi::Service::Access::Write,
          Tsugumi::Service::Access::Run}) {
      auto asked = db.prepare(
          "SELECT 1 FROM access_requests r JOIN access_prompt_items i ON "
          "i.request=r.id "
          "WHERE r.project=? AND i.access=? "
          "UNION ALL SELECT 1 FROM access_requests WHERE project=? AND "
          "access=?");
      asked.bind(1, project)
          .bind(2, static_cast<int>(candidate))
          .bind(3, project)
          .bind(4, static_cast<int>(candidate));
      if (candidate != access &&
          (HasGrant(project, conversation, candidate) || asked.step()))
        continue;
      auto item = db.prepare("INSERT INTO access_prompt_items VALUES(?,?)");
      item.bind(1, id).bind(2, static_cast<int>(candidate)).run();
    }
  }
  std::vector<std::string> asked;
  for (const auto& description :
       AccessDescriptions(id, project, access, reason, per_call))
    asked.push_back(json::Quote(description));
  tx.commit();
  dependencies.event(conversation, json::Encode(json::Members{
                                       {"kind", json::Quote("permission")},
                                       {"id", json::Quote(id)},
                                       {"asked", json::Encode(asked)}}));
  return id;
}

auto Access::Require(const std::string& project,
                     const std::string& conversation,
                     Tsugumi::Service::Access access, bool wait,
                     std::stop_token stop, const std::string& reason,
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
      RequestAccess(project, conversation, access, reason, false, true);
  if (!wait)
    throw Failure("grant_required", "Explicit user permission is required", id);
  changed.wait(lock, stop, [&] {
    if (stop.stop_requested() || stopping) throw Cancelled();
    if (HasGrant(project, conversation, access)) return true;
    auto q = db.prepare("SELECT state FROM access_requests WHERE id=?");
    q.bind(1, id);
    if (!q.step() || q.text(0) == "denied")
      throw Failure("permission_denied", "The user declined access");
    return false;
  });
  if (stop.stop_requested()) throw Cancelled();
}

auto Access::AuthorizeContext(const std::string& id,
                              const std::vector<Message>& messages) -> void {
  std::lock_guard lock(mutex);
  for (const auto& resource : agent::Resources(messages))
    if (!HasGrant(resource, id, Tsugumi::Service::Access::Read))
      throw Failure("permission_denied",
                    "The conversation contains content whose permission "
                    "was revoked; clear it before continuing");
}

auto Access::Revoke(const std::string& resource, S::Access access) -> void {
  auto q =
      db.prepare("UPDATE grants SET active=0 WHERE project=? AND access=?");
  q.bind(1, resource).bind(2, static_cast<int>(access)).run();
  if (access == S::Access::Read || access == S::Access::Write ||
      access == S::Access::Run || access == S::Access::Shell) {
    auto owner = db.prepare("SELECT project FROM folders WHERE id=?");
    owner.bind(1, resource);
    dependencies.cancel(owner.step() ? owner.text(0) : resource, "");
  }
  changed.notify_all();
}

auto Access::HandleHost(const Tsugumi::Host::Request& request, uint64_t id)
    -> std::string {
  namespace H = Tsugumi::Host;
  if (const auto* edit = request.value_as_ProjectPermissionSetRequest()) {
    const auto project = edit->project_id()->str();
    const auto resource = edit->resource_id()->str();
    auto known = db.prepare("SELECT 1 FROM projects WHERE id=?");
    known.bind(1, project);
    if (!known.step()) throw Failure("not_found", "Project not found");
    if (resource != project) {
      auto folder =
          db.prepare("SELECT 1 FROM folders WHERE id=? AND project=?");
      folder.bind(1, resource).bind(2, project);
      if (!folder.step())
        throw Failure("not_found", "Attached folder not found");
      if (edit->access() != S::Access::Read)
        throw Failure("invalid_request",
                      "Attached folders allow read access only");
    } else if (edit->access() != S::Access::Read &&
               edit->access() != S::Access::Write &&
               edit->access() != S::Access::Run &&
               edit->access() != S::Access::Shell) {
      throw Failure("invalid_request",
                    "This access requires an operation approval");
    }
    if (!edit->allow()) {
      Revoke(resource, edit->access());
    } else if (!HasGrant(resource, "", edit->access())) {
      auto grant = db.prepare("INSERT INTO grants VALUES(?,?,?,1)");
      grant.bind(1, crypto::uuid4())
          .bind(2, resource)
          .bind(3, static_cast<int>(edit->access()))
          .run();
      changed.notify_all();
    }
  }
  H::ProjectPermissionListT result;
  auto projects = db.prepare(
      "SELECT id,title,coalesce((SELECT path FROM "
      "project_paths WHERE id=projects.id),'') "
      "FROM projects ORDER BY title,id");
  while (projects.step()) {
    auto project = std::make_unique<H::ProjectPermissionsT>();
    project->project_id = projects.text(0);
    project->title = projects.text(1);
    project->path = projects.text(2);
    const auto add = [&](const std::string& resource, const std::string& title,
                         const std::string& path, S::Access access) {
      auto permission = std::make_unique<H::ProjectPermissionT>();
      permission->resource_id = resource;
      permission->title = title;
      permission->path = path;
      permission->access = access;
      permission->allowed = HasGrant(resource, "", access);
      project->permissions.push_back(std::move(permission));
    };
    for (const auto access :
         {S::Access::Read, S::Access::Write, S::Access::Run, S::Access::Shell})
      add(project->project_id, project->title, project->path, access);
    auto folders = db.prepare(
        "SELECT id,title,path FROM folders WHERE project=? ORDER BY path");
    folders.bind(1, project->project_id);
    while (folders.step())
      add(folders.text(0), folders.text(1), folders.text(2), S::Access::Read);
    result.projects.push_back(std::move(project));
  }
  return wire::HostReply(id, std::move(result));
}

auto Access::Handle(const RequestIn& request, bool human) -> std::string {
  const auto* r = A::GetMessage(request.frame->data())->payload_as_Request();
  switch (r->value_type()) {
    case A::RequestPayload::AccessRequest: {
      auto p = r->value_as_AccessRequest();
      if (p->access() > Tsugumi::Service::Access::MAX)
        throw Failure("invalid_request", "Invalid permission access");
      const auto conversation = Text(p->conversation_id());
      if (!conversation.empty()) conversations->GetConversation(conversation);
      if (!conversation.empty())
        conversations->OwnConversation(conversation, human);
      const auto id = RequestAccess(p->project_id()->str(), conversation,
                                    p->access(), Text(p->reason()));
      A::AccessResultT result;
      result.id = id;
      return wire::AgentReply(request.request_id, std::move(result));
    }
    case A::RequestPayload::AccessResolveRequest: {
      Human(human);
      auto p = r->value_as_AccessResolveRequest();
      auto q = db.prepare(
          "SELECT project,conversation,access,state FROM "
          "access_requests WHERE id=?");
      q.bind(1, p->request_id()->str());
      if (!q.step()) throw Failure("not_found", "Access request not found");
      if (q.text(3) != "pending")
        throw Failure("conflict", "Access request was already answered");
      sql::Transaction tx(db);
      auto operation =
          db.prepare("SELECT 1 FROM operation_approvals WHERE request=?");
      operation.bind(1, p->request_id()->str());
      const bool per_call =
          operation.step() || static_cast<Tsugumi::Service::Access>(q.i64(2)) ==
                                  Tsugumi::Service::Access::HelperScope;
      const auto needed = static_cast<S::Access>(q.i64(2));
      const auto chosen = [&](S::Access access) {
        if (!p->allow()) return false;
        if (per_call || !p->selected() || p->selected()->size() == 0)
          return access == needed;
        return std::ranges::find(*p->selected(), access) !=
               p->selected()->end();
      };
      const bool allow = chosen(needed);
      std::set<S::Access> offered{needed};
      auto items =
          db.prepare("SELECT access FROM access_prompt_items WHERE request=?");
      items.bind(1, p->request_id()->str());
      while (items.step()) offered.insert(static_cast<S::Access>(items.i64(0)));
      if (!per_call)
        for (const auto permission : offered) {
          if (!chosen(permission)) continue;
          auto grant = db.prepare("INSERT INTO grants VALUES(?,?,?,1)");
          grant
              .bind(1, permission == needed ? p->request_id()->str()
                                            : crypto::uuid4())
              .bind(2, q.text(0))
              .bind(3, static_cast<int>(permission))
              .run();
        }
      auto update = db.prepare("UPDATE access_requests SET state=? WHERE id=?");
      update.bind(1, allow ? "allowed" : "denied")
          .bind(2, p->request_id()->str())
          .run();
      tx.commit();
      dependencies.event(
          q.text(1),
          json::Encode(json::Members{
              {"kind", json::Quote("permissionAnswered")},
              {"id", json::Quote(p->request_id()->str())},
              {"ok", allow ? "true" : "false"},
              {"text", json::Quote(allow ? "Approved this request"
                                         : "Declined; nothing changed")}}));
      changed.notify_all();
      S::AcknowledgementT result;
      result.accepted = allow;
      return wire::AgentReply(request.request_id, result);
    }
    case A::RequestPayload::AccessRevokeRequest: {
      Human(human);
      const auto grant_id =
          r->value_as_AccessRevokeRequest()->grant_id()->str();
      auto grant = db.prepare(
          "SELECT project,access FROM grants WHERE id=? AND active=1");
      grant.bind(1, grant_id);
      if (grant.step())
        Revoke(grant.text(0), static_cast<S::Access>(grant.i64(1)));
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
          "SELECT id,project,conversation,access,reason,state,"
          "EXISTS(SELECT 1 FROM operation_approvals WHERE "
          "request=access_requests.id) "
          "FROM access_requests WHERE (?='' OR conversation=?) "
          "ORDER BY rowid DESC LIMIT 500");
      q.bind(1, id).bind(2, id);
      while (q.step()) {
        auto entry = std::make_unique<A::AccessEntryT>();
        entry->id = q.text(0);
        entry->project_id = q.text(1);
        entry->conversation_id = q.text(2);
        entry->access = static_cast<Tsugumi::Service::Access>(q.i64(3));
        entry->reason = q.text(4);
        entry->state = q.text(5);
        entry->asked = AccessDescriptions(
            entry->id, entry->project_id, entry->access, entry->reason,
            q.i64(6) || entry->access == S::Access::HelperScope);
        entry->groups = agent::AccessGroups(entry->asked);
        result.requests.push_back(std::move(entry));
      }
      auto g = db.prepare(
          "SELECT id,project,access,active FROM grants "
          "WHERE (?='' OR project=(SELECT project FROM conversation_locations "
          "WHERE conversation=?) OR project IN (SELECT id FROM folders WHERE "
          "project=(SELECT project FROM conversation_locations WHERE "
          "conversation=?)))");
      g.bind(1, id).bind(2, id).bind(3, id);
      while (g.step()) {
        auto grant = std::make_unique<A::GrantT>();
        grant->id = g.text(0);
        grant->project_id = g.text(1);
        grant->access = static_cast<Tsugumi::Service::Access>(g.i64(2));
        grant->active = g.i64(3) != 0;
        result.grants.push_back(std::move(grant));
      }
      return wire::AgentReply(request.request_id, std::move(result));
    }
    default:
      throw Failure("invalid_request", "Unsupported access request");
  }
}
}  // namespace tsugumi::host
