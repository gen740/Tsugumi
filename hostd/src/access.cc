#include "host/access.hh"

#include <algorithm>
#include <array>

#include "agent/context.hh"
#include "agent/presentation.hh"
#include "agent/store.hh"
#include "agent/wire.hh"
#include "host/failure.hh"
#include "host/projects.hh"
#include "host/wire_helpers.hh"
#include "tsugumi/rpc/transport.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/json_object.hh"

namespace tsugumi::host {
namespace A = Tsugumi::Agent;
namespace S = Tsugumi::Service;
namespace json = ::tsugumi::json;
namespace wire = agent::wire;
using agent::Cancelled;
using agent::Message;
namespace {
// Reserve room for the reply envelope and vector headers. Each independent
// packing below also budgets the vector offset and worst-case alignment.
constexpr auto kAccessPageBytes = TransportLimits{}.max_frame_bytes - 1024;
template <typename Table, typename Entry>
auto AccessEntryBytes(flatbuffers::FlatBufferBuilder& builder,
                      const Entry& entry) -> size_t {
  builder.Clear();
  Table::Pack(builder, &entry);
  return builder.GetSize() + sizeof(flatbuffers::uoffset_t) + alignof(uint64_t);
}

constexpr std::array kResidentAccess{S::Access::Read, S::Access::Write,
                                     S::Access::Run, S::Access::Shell};
auto ResidentAccess(S::Access access) -> bool {
  return std::ranges::find(kResidentAccess, access) != kResidentAccess.end();
}
auto AccessBit(S::Access access) -> int {
  return 1 << static_cast<unsigned>(access);
}
auto ApprovalDescription(ApprovalKind kind, const std::string& preview)
    -> std::string {
  return (kind == ApprovalKind::Turn ? "project_access helper_scope\n"
                                     : "project_access operation_approval\n") +
         preview;
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

auto Access::HasGrant(const std::string& project, S::Access access) -> bool {
  if (project.empty()) return true;
  auto q = db.prepare(
      "SELECT 1 FROM workspace_grants WHERE project=? AND access=? AND "
      "active=1 LIMIT 1");
  q.bind(1, project).bind(2, static_cast<int>(access));
  return q.step();
}

auto Access::AccessDescription(const std::string& resource,
                               Tsugumi::Service::Access access,
                               const std::string& reason, bool needed)
    -> std::string {
  auto q = db.prepare(
      "SELECT coalesce((SELECT title FROM projects WHERE id=?1),"
      "(SELECT title FROM folders WHERE id=?1),''),"
      "coalesce((SELECT path FROM project_paths WHERE id=?1),"
      "(SELECT path FROM folders WHERE id=?1),'')");
  q.bind(1, resource).step();
  auto title = q.text(0);
  const auto path = q.text(1);
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

auto Access::AccessDescriptions(const std::string& project, S::Access needed,
                                const std::string& reason, int offered)
    -> std::vector<std::string> {
  std::vector<std::string> asked{AccessDescription(project, needed, reason)};
  for (const auto access : kResidentAccess)
    if (access != needed && (offered & AccessBit(access)))
      asked.push_back(AccessDescription(project, access, reason, false));
  return asked;
}

auto Access::RequestAccess(const std::string& project,
                           const std::string& conversation,
                           Tsugumi::Service::Access access,
                           const std::string& reason, bool combine)
    -> std::string {
  if (!ResidentAccess(access))
    throw Failure("invalid_request", "Invalid resident permission");
  auto folder = db.prepare("SELECT project FROM folders WHERE id=?");
  folder.bind(1, project);
  registry.Lookup(folder.step() ? folder.text(0) : project);
  auto old = db.prepare(
      "SELECT id FROM permission_requests WHERE project=? AND conversation=? "
      "AND "
      "access=? AND state='pending'");
  old.bind(1, project).bind(2, conversation).bind(3, static_cast<int>(access));
  if (old.step()) return old.text(0);
  int offered = AccessBit(access);
  // Only ordinary project access is bundled. Folder access, shell access and
  // per-operation approvals retain their own, narrower questions.
  if (combine && access <= Tsugumi::Service::Access::Run &&
      !conversation.empty() &&
      conversations->GetConversation(conversation).project == project) {
    for (const auto candidate : kResidentAccess) {
      if (candidate == S::Access::Shell || HasGrant(project, candidate))
        continue;
      auto asked = db.prepare(
          "SELECT 1 FROM permission_requests WHERE project=? AND offered & ?");
      asked.bind(1, project).bind(2, AccessBit(candidate));
      if (!asked.step()) offered |= AccessBit(candidate);
    }
  }
  const auto id = crypto::uuid4();
  auto q = db.prepare(
      "INSERT INTO "
      "permission_requests(id,project,conversation,access,reason,state,offered)"
      " "
      "VALUES(?,?,?,?,?,'pending',?)");
  q.bind(1, id)
      .bind(2, project)
      .bind(3, conversation)
      .bind(4, static_cast<int>(access))
      .bind(5, reason)
      .bind(6, offered)
      .run();
  PublishRequest(conversation, id,
                 AccessDescriptions(project, access, reason, offered));
  return id;
}

auto Access::PublishRequest(const std::string& conversation,
                            const std::string& id,
                            const std::vector<std::string>& descriptions,
                            const std::string& call_id) -> void {
  std::vector<std::string> asked;
  for (const auto& description : descriptions)
    asked.push_back(json::Quote(description));
  dependencies.event(conversation, json::Encode(json::Members{
                                       {"kind", json::Quote("permission")},
                                       {"id", json::Quote(id)},
                                       {"call_id", json::Quote(call_id)},
                                       {"asked", json::Encode(asked)}}));
}

auto Access::FindApproval(ApprovalKind kind, const std::string& conversation,
                          const std::string& call, const std::string& digest)
    -> std::optional<Approval> {
  auto q = db.prepare(
      "SELECT id,reason,digest FROM permission_requests "
      "WHERE conversation=? AND access=? AND call=?");
  q.bind(1, conversation).bind(2, static_cast<int>(kind)).bind(3, call);
  if (!q.step()) return std::nullopt;
  if (q.text(2) != digest)
    throw Failure("conflict",
                  "An approval cannot be used with different arguments");
  return Approval{q.text(0), q.text(1)};
}

auto Access::RequestApproval(ApprovalKind kind, const std::string& project,
                             const std::string& conversation,
                             const std::string& call, const std::string& digest,
                             const std::string& preview) -> Approval {
  if (auto old = FindApproval(kind, conversation, call, digest)) return *old;
  const auto id = crypto::uuid4();
  auto put = db.prepare(
      "INSERT INTO "
      "permission_requests(id,project,conversation,access,reason,state,call,"
      "digest) "
      "VALUES(?,?,?,?,?,'pending',?,?)");
  put.bind(1, id)
      .bind(2, project)
      .bind(3, conversation)
      .bind(4, static_cast<int>(kind))
      .bind(5, preview)
      .bind(6, call)
      .bind(7, digest)
      .run();
  if (kind == ApprovalKind::Turn)
    PublishRequest(conversation, id, {});
  else
    PublishRequest(conversation, id, {ApprovalDescription(kind, preview)},
                   call);
  return {id, preview};
}

// The conversation database is authoritative across interruption and restart.
auto Access::TurnPending(const std::string& conversation,
                         const std::string& turn, const std::string& digest)
    -> bool {
  auto q =
      conversations->ConversationDatabase(conversation)
          .prepare(
              "SELECT 1 FROM conversation_turns WHERE conversation=? AND id=? "
              "AND approval_digest=? AND state IN ('running','interrupted')");
  q.bind(1, conversation).bind(2, turn).bind(3, digest);
  return q.step();
}

auto Access::CancelTurnApproval(const std::string& conversation,
                                const std::string& turn) -> void {
  auto q = db.prepare(
      "UPDATE permission_requests SET state='cancelled' "
      "WHERE conversation=? AND access=? AND call=? AND state IN "
      "('pending','allowed') "
      "RETURNING id");
  q.bind(1, conversation)
      .bind(2, static_cast<int>(ApprovalKind::Turn))
      .bind(3, turn);
  std::vector<std::string> ids;
  while (q.step()) ids.push_back(q.text(0));
  for (const auto& id : ids)
    dependencies.event(
        conversation,
        json::Encode(json::Members{{"kind", json::Quote("permissionAnswered")},
                                   {"id", json::Quote(id)},
                                   {"ok", "false"}}));
}

auto Access::WaitApproval(std::unique_lock<std::recursive_mutex>& lock,
                          const std::string& id, bool wait,
                          std::stop_token stop) -> void {
  changed.wait(lock, stop, [&] {
    if (stop.stop_requested() || stopping) throw Cancelled();
    auto query =
        db.prepare("SELECT state,reason FROM permission_requests WHERE id=?");
    query.bind(1, id);
    if (!query.step()) throw Failure("not_found", "Approval request not found");
    const auto state = query.text(0);
    if (state == "denied")
      throw Failure("permission_denied", "The user declined this request");
    if (state == "allowed") return true;
    if (state != "pending")
      throw Failure(
          "outcome_unknown",
          "This approval was already consumed; consult the recorded outcome");
    if (!wait) throw Failure("grant_required", query.text(1), id);
    return false;
  });
  if (stop.stop_requested()) throw Cancelled();
}

auto Access::AuthorizeTurn(const std::string& project,
                           const std::string& conversation,
                           const std::string& turn, const std::string& digest,
                           const std::string& preview, std::stop_token stop)
    -> TurnAuthorization {
  std::unique_lock lock(mutex);
  if (stop.stop_requested() || stopping ||
      !TurnPending(conversation, turn, digest))
    throw Cancelled();
  registry.Lookup(project);
  const auto approval =
      RequestApproval(ApprovalKind::Turn, project, conversation, turn, digest,
                      json::Encode(json::Members{{"task", preview}}));
  WaitApproval(lock, approval.id, true, stop);
  return TurnAuthorization(project, conversation);
}

auto Access::Require(const std::string& project,
                     const std::string& conversation,
                     Tsugumi::Service::Access access, bool wait,
                     std::stop_token stop, const std::string& reason,
                     const std::optional<TurnAuthorization>& authorization)
    -> void {
  std::unique_lock lock(mutex);
  if (authorization) {
    if (stop.stop_requested() || stopping) throw Cancelled();
    if (access <= S::Access::Run &&
        authorization->Matches(project, conversation))
      return;
    throw Failure("permission_denied",
                  "The turn does not authorize this access");
  }
  if (HasGrant(project, access)) return;
  const auto id = RequestAccess(project, conversation, access, reason, true);
  if (!wait)
    throw Failure("grant_required", "Explicit user permission is required", id);
  changed.wait(lock, stop, [&] {
    if (stop.stop_requested() || stopping) throw Cancelled();
    if (HasGrant(project, access)) return true;
    auto q = db.prepare("SELECT state FROM permission_requests WHERE id=?");
    q.bind(1, id);
    if (!q.step() || q.text(0) == "denied")
      throw Failure("permission_denied", "The user declined access");
    return false;
  });
  if (stop.stop_requested()) throw Cancelled();
}

auto Access::AuthorizeContext(
    const std::vector<Message>& messages,
    const std::optional<TurnAuthorization>& authorization) -> void {
  std::lock_guard lock(mutex);
  for (const auto& resource : agent::Resources(messages))
    if (!(authorization && authorization->project() == resource) &&
        !HasGrant(resource, S::Access::Read))
      throw Failure("permission_denied",
                    "The conversation contains content whose permission "
                    "was revoked; clear it before continuing");
}

auto Access::Revoke(const std::string& resource, S::Access access) -> void {
  auto q = db.prepare(
      "UPDATE workspace_grants SET active=0 WHERE project=? AND access=?");
  q.bind(1, resource).bind(2, static_cast<int>(access)).run();
  auto owner = db.prepare("SELECT project FROM folders WHERE id=?");
  owner.bind(1, resource);
  dependencies.cancel(owner.step() ? owner.text(0) : resource);
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
    } else if (!ResidentAccess(edit->access())) {
      throw Failure("invalid_request",
                    "This access requires an operation approval");
    }
    if (!edit->allow()) {
      Revoke(resource, edit->access());
    } else if (!HasGrant(resource, edit->access())) {
      auto grant = db.prepare("INSERT INTO workspace_grants VALUES(?,?,?,1)");
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
      permission->allowed = HasGrant(resource, access);
      project->permissions.push_back(std::move(permission));
    };
    for (const auto access : kResidentAccess)
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
      const auto id = p->request_id()->str();
      auto q = db.prepare(
          "SELECT project,conversation,access,state,call,offered,digest FROM "
          "permission_requests "
          "WHERE id=?");
      q.bind(1, id);
      if (!q.step()) throw Failure("not_found", "Access request not found");
      if (q.text(3) != "pending")
        throw Failure("conflict", "Access request was already answered");
      const auto conversation = q.text(1);
      const auto needed = static_cast<S::Access>(q.i64(2));
      if (needed == S::Access::HelperScope &&
          !TurnPending(conversation, q.text(4), q.text(6)))
        throw Failure("conflict", "The turn no longer needs approval");
      bool allow = p->allow();
      sql::Transaction tx(db);
      if (ResidentAccess(needed)) {
        const auto chosen = [&](S::Access access) {
          if (!p->allow()) return false;
          if (!p->selected() || p->selected()->size() == 0)
            return access == needed;
          return std::ranges::find(*p->selected(), access) !=
                 p->selected()->end();
        };
        allow = chosen(needed);
        for (const auto permission : kResidentAccess) {
          if (!(q.i64(5) & AccessBit(permission)) || !chosen(permission))
            continue;
          auto grant =
              db.prepare("INSERT INTO workspace_grants VALUES(?,?,?,1)");
          grant.bind(1, permission == needed ? id : crypto::uuid4())
              .bind(2, q.text(0))
              .bind(3, static_cast<int>(permission))
              .run();
        }
      }
      auto update =
          db.prepare("UPDATE permission_requests SET state=? WHERE id=?");
      update.bind(1, allow ? "allowed" : "denied").bind(2, id).run();
      tx.commit();
      changed.notify_all();
      dependencies.event(
          conversation,
          json::Encode(json::Members{
              {"kind", json::Quote("permissionAnswered")},
              {"id", json::Quote(id)},
              {"ok", allow ? "true" : "false"},
              {"text", json::Quote(allow ? "Approved this request"
                                         : "Declined; nothing changed")}}));
      S::AcknowledgementT result;
      result.accepted = allow;
      return wire::AgentReply(request.request_id, result);
    }
    case A::RequestPayload::AccessRevokeRequest: {
      Human(human);
      const auto grant_id =
          r->value_as_AccessRevokeRequest()->grant_id()->str();
      auto grant = db.prepare(
          "SELECT project,access FROM workspace_grants WHERE id=? AND "
          "active=1");
      grant.bind(1, grant_id);
      if (grant.step())
        Revoke(grant.text(0), static_cast<S::Access>(grant.i64(1)));
      return wire::AgentReply(request.request_id, S::AcknowledgementT{});
    }
    case A::RequestPayload::AccessListRequest: {
      const auto* filter = r->value_as_AccessListRequest();
      const auto id = Text(filter->conversation_id());
      const auto request_id = Text(filter->request_id());
      if (!human && id.empty())
        throw Failure("permission_denied",
                      "External clients must name their conversation");
      if (!id.empty()) {
        conversations->OwnConversation(id, human);
        if (!human) conversations->AuthorizeHistory(id);
      }
      A::AccessListT result;
      const auto project = Text(filter->project_id());
      const auto cursor = filter->cursor();
      const bool pending_only = filter->pending_only();
      size_t remaining = kAccessPageBytes;
      flatbuffers::FlatBufferBuilder measure;
      // Positive cursors continue access requests; negative ones continue
      // grants. Both name the first row not yet returned.
      if (cursor >= 0) {
        auto q = db.prepare(
            "SELECT "
            "rowid,id,project,conversation,access,reason,state,call,offered,"
            "digest "
            "FROM permission_requests WHERE (?1='' OR conversation=?1) "
            "AND (?2='' OR id=?2) "
            "AND (?3='' OR project=?3 OR project IN (SELECT id FROM folders "
            "WHERE project=?3) "
            "OR conversation IN (SELECT conversation FROM "
            "conversation_locations WHERE project=?3)) "
            "AND (NOT ?4 OR state='pending') AND (?5=0 OR rowid<=?5) "
            "AND (?6 OR access<>?7) ORDER BY rowid DESC");
        q.bind(1, id)
            .bind(2, request_id)
            .bind(3, project)
            .bind(4, pending_only)
            .bind(5, cursor)
            .bind(6, human)
            .bind(7, static_cast<int>(ApprovalKind::Turn));
        while (q.step()) {
          auto entry = std::make_unique<A::AccessEntryT>();
          entry->id = q.text(1);
          entry->project_id = q.text(2);
          entry->conversation_id = q.text(3);
          entry->access = static_cast<S::Access>(q.i64(4));
          entry->state = q.text(6);
          if (ResidentAccess(entry->access)) {
            entry->asked = AccessDescriptions(entry->project_id, entry->access,
                                              q.text(5), q.i64(8));
          } else {
            const auto kind = static_cast<ApprovalKind>(entry->access);
            if (kind == ApprovalKind::Operation) entry->call_id = q.text(7);
            if (kind == ApprovalKind::Turn && entry->state == "pending") {
              try {
                if (!TurnPending(entry->conversation_id, q.text(7), q.text(9)))
                  continue;
              } catch (const std::exception& error) {
                // Other projects remain listable when this conversation is
                // unavailable. Resolving its approval still checks the turn.
                entry->state = "unavailable";
                entry->unavailable_reason = error.what();
              }
            }
            if (entry->state != "unavailable")
              entry->asked = {ApprovalDescription(kind, q.text(5))};
          }
          entry->groups = agent::AccessGroups(entry->asked);
          auto bytes = AccessEntryBytes<A::AccessEntry>(measure, *entry);
          if (bytes > kAccessPageBytes) {
            entry->state = "unavailable";
            entry->unavailable_reason =
                "The approval exceeds the RPC frame size";
            entry->asked.clear();
            entry->groups.clear();
            bytes = AccessEntryBytes<A::AccessEntry>(measure, *entry);
            if (bytes > kAccessPageBytes)
              throw Failure(
                  "resource_exhausted",
                  "Access request metadata exceeds the RPC frame size");
          }
          if (bytes > remaining) {
            result.next_cursor = q.i64(0);
            break;
          }
          remaining -= bytes;
          result.requests.push_back(std::move(entry));
        }
      }
      if (!request_id.empty() || pending_only || result.next_cursor)
        return wire::AgentReply(request.request_id, std::move(result));
      auto g = db.prepare(
          "SELECT rowid,id,project,access,active FROM workspace_grants "
          "WHERE (?1='' OR project=(SELECT project FROM conversation_locations "
          "WHERE conversation=?1) OR project IN (SELECT id FROM folders WHERE "
          "project=(SELECT project FROM conversation_locations WHERE "
          "conversation=?1))) "
          "AND (?2='' OR project=?2 OR project IN (SELECT id FROM folders "
          "WHERE project=?2)) "
          "AND (?3>=0 OR -rowid>=?3) ORDER BY rowid DESC");
      g.bind(1, id).bind(2, project).bind(3, cursor);
      while (g.step()) {
        auto grant = std::make_unique<A::GrantT>();
        grant->id = g.text(1);
        grant->project_id = g.text(2);
        grant->access = static_cast<S::Access>(g.i64(3));
        grant->active = g.i64(4) != 0;
        const auto bytes = AccessEntryBytes<A::Grant>(measure, *grant);
        if (bytes > kAccessPageBytes)
          throw Failure("resource_exhausted",
                        "Workspace grant exceeds the RPC frame size");
        if (bytes > remaining) {
          result.next_cursor = -g.i64(0);
          break;
        }
        remaining -= bytes;
        result.grants.push_back(std::move(grant));
      }
      return wire::AgentReply(request.request_id, std::move(result));
    }
    default:
      throw Failure("invalid_request", "Unsupported access request");
  }
}
}  // namespace tsugumi::host
