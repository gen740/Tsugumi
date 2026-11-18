#include "agent/approvals.hh"

#include "agent/approval.hh"
#include "agent/store.hh"
#include "agent/tools.hh"
#include "host/access.hh"
#include "host/failure.hh"
#include "tsugumi/util/crypto.hh"

namespace tsugumi::agent {
namespace A = Tsugumi::Agent;
using host::Failure;
Approvals::Approvals(host::StateGate& gate, sql::Db& db,
                     ConversationStore& conversations, Tools& tools,
                     bool& stopping, std::condition_variable_any& changed,
                     host::Access& access)
    : mutex(gate.mutex),
      db(db),
      conversations(&conversations),
      tools(&tools),
      stopping(stopping),
      changed(changed),
      access_service(access) {}

auto Approvals::ApproveOperation(
    const std::string& project, const std::string& conversation,
    const ToolCall& call, const std::string& digest, const json::Members& args,
    bool wait, std::stop_token stop,
    const std::vector<std::string>& permissions) -> std::vector<std::string> {
  std::vector<std::string> run_ids;
  const auto binding =
      permissions.empty()
          ? digest
          : crypto::sha256_hex(digest + "\n" + json::Encode(permissions));
  auto describe = [&] {
    if (!permissions.empty())
      return std::string("operation_approval\n") + call.name + "\n" +
             json::Encode(
                 json::Members{{"project", json::Quote(project)},
                               {"arguments_digest", json::Quote(digest)},
                               {"permissions", json::Encode(permissions)}});
    return DescribeOperation(
        call.name, args, project,
        [&](const std::string& name, const json::Members& query) {
          const auto reply = json::Object(
              tools->ProjectCall(project, name, json::Encode(query), stop));
          if (json::Text(reply, "reply") == "Failure")
            throw Failure(
                "approval_unavailable",
                "Cannot read approval targets: " + json::Encode(reply));
          return json::Object(json::Get(reply, "value", "{}"));
        },
        &run_ids);
  };
  std::unique_lock lock(mutex);
  auto old = db.prepare(
      "SELECT digest,request,preview FROM operation_approvals WHERE "
      "conversation=? AND call=?");
  old.bind(1, conversation).bind(2, call.id);
  std::string request, preview;
  if (old.step()) {
    if (old.text(0) != binding)
      throw Failure("conflict",
                    "An approval cannot be used with different arguments");
    request = old.text(1);
    preview = old.text(2);
  } else {
    lock.unlock();
    preview = describe();
    lock.lock();
    if (!access_service.HasGrant(project, conversation,
                                 Tsugumi::Service::Access::Read))
      throw Failure("permission_denied",
                    "Access was revoked while reading approval targets");
    conversations->RememberResource(conversation, project);
    // Another connection can submit the same call while targets are read.
    auto raced = db.prepare(
        "SELECT digest,request,preview FROM operation_approvals WHERE "
        "conversation=? AND call=?");
    raced.bind(1, conversation).bind(2, call.id);
    if (raced.step()) {
      if (raced.text(0) != binding)
        throw Failure("conflict",
                      "An approval cannot be used with different arguments");
      request = raced.text(1);
      preview = raced.text(2);
    } else {
      // Reuse AccessRequest/AccessResolveRequest; this request creates no
      // grant. The call ID prevents coalescing identical but independent acts.
      request = access_service.RequestAccess(
          project, conversation, Tsugumi::Service::Access::CardPermissions,
          preview + "\nCall: " + json::Quote(call.id), true);
      auto put =
          db.prepare("INSERT INTO operation_approvals VALUES(?,?,?,?,?)");
      put.bind(1, conversation)
          .bind(2, call.id)
          .bind(3, binding)
          .bind(4, request)
          .bind(5, preview)
          .run();
    }
  }
  for (;;) {
    auto state = db.prepare("SELECT state FROM access_requests WHERE id=?");
    state.bind(1, request);
    if (!state.step() || state.text(0) == "denied")
      throw Failure("permission_denied",
                    "The user declined this operation. Nothing was changed; "
                    "continue without retrying it.");
    if (state.text(0) == "allowed") break;
    if (state.text(0) != "pending")
      throw Failure(
          "outcome_unknown",
          "This approval was already consumed; consult the recorded outcome");
    if (!wait) throw Failure("grant_required", preview, request);
    if (stop.stop_requested() || stopping) throw Cancelled();
    changed.wait_for(lock, std::chrono::milliseconds(100));
  }
  lock.unlock();
  const auto current = describe();
  lock.lock();
  if (!access_service.HasGrant(project, conversation,
                               Tsugumi::Service::Access::Read) ||
      !access_service.HasGrant(project, conversation,
                               Tsugumi::Service::Access::Write))
    throw Failure("permission_denied",
                  "Project access was revoked before approval could be used");
  if (current != preview) {
    auto stale = db.prepare(
        "UPDATE access_requests SET state='stale' WHERE id=? AND "
        "state='allowed'");
    stale.bind(1, request).run();
    throw Failure("conflict",
                  "Approval targets changed. Nothing was executed; propose a "
                  "new call for approval.");
  }
  auto consume = db.prepare(
      "UPDATE access_requests SET state='consumed' WHERE id=? AND "
      "state='allowed'");
  consume.bind(1, request).run();
  if (!db.changes())
    throw Failure("outcome_unknown", "This approval was already consumed");
  return run_ids;
}

}  // namespace tsugumi::agent
