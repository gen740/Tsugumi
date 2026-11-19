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
                     host::Access& access)
    : mutex(gate.mutex),
      db(db),
      conversations(&conversations),
      tools(&tools),
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
      return call.name + "\n" +
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
  std::string preview;
  {
    auto old = db.prepare(
        "SELECT preview FROM approvals WHERE conversation=? AND "
        "kind='operation' AND call=?");
    old.bind(1, conversation).bind(2, call.id);
    if (old.step()) preview = old.text(0);
  }
  if (preview.empty()) {
    lock.unlock();
    preview = describe();
    lock.lock();
  }
  conversations->RememberResource(conversation, project);
  const auto request =
      access_service.RequestApproval(host::ApprovalKind::Operation, project,
                                     conversation, call.id, binding, preview);
  {
    auto stored = db.prepare("SELECT preview FROM approvals WHERE id=?");
    stored.bind(1, request);
    stored.step();
    preview = stored.text(0);
  }
  access_service.WaitApproval(lock, request, wait, stop);
  lock.unlock();
  const auto current = describe();
  lock.lock();
  if (current != preview) {
    auto stale = db.prepare(
        "UPDATE approvals SET state='stale' WHERE id=? AND "
        "state='allowed'");
    stale.bind(1, request).run();
    throw Failure("conflict",
                  "Approval targets changed. Nothing was executed; propose a "
                  "new call for approval.");
  }
  auto consume = db.prepare(
      "UPDATE approvals SET state='consumed' WHERE id=? AND "
      "state='allowed'");
  consume.bind(1, request).run();
  if (!db.changes())
    throw Failure("outcome_unknown", "This approval was already consumed");
  return run_ids;
}

}  // namespace tsugumi::agent
