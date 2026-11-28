#include "host/decisions.hh"

#include "agent/store.hh"
#include "agent/wire.hh"
#include "host/failure.hh"
#include "host/wire_helpers.hh"
#include "tsugumi/rpc/transport.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/json_object.hh"
#include "tsugumi/util/locked.hh"

namespace tsugumi::host {
namespace A = Tsugumi::Agent;
namespace S = Tsugumi::Service;
namespace json = ::tsugumi::json;
namespace wire = agent::wire;
using agent::Cancelled;
namespace {
// Reserve room for the reply envelope and vector headers. Each entry below
// also budgets the vector offset and worst-case alignment.
constexpr auto kPageBytes = TransportLimits{}.max_frame_bytes - 1024;
auto EntryBytes(flatbuffers::FlatBufferBuilder& builder,
                const A::DecisionT& entry) -> size_t {
  builder.Clear();
  builder.Finish(A::Decision::Pack(builder, &entry));
  return builder.GetSize() + sizeof(flatbuffers::uoffset_t) + alignof(uint64_t);
}
}  // namespace

Decisions::Decisions(StateGate& gate, sql::Db& db, ProjectRegistry& registry,
                     agent::ConversationStore& conversations, bool& stopping,
                     Dependencies dependencies)
    : mutex(gate.mutex),
      db(db),
      registry(registry),
      conversations(&conversations),
      stopping(stopping),
      dependencies(std::move(dependencies)) {}

auto Decisions::Summary(const std::string& preview) -> std::string {
  const auto end = preview.find('\n');
  const auto name = preview.substr(0, end);
  if (end == std::string::npos) return name;
  const auto next = preview.find('\n', end + 1);
  json::Members targets;
  try {
    targets = json::Object(preview.substr(end + 1, next - end - 1));
  } catch (const std::exception&) {
    return name;
  }
  if (targets.contains("permissions")) {
    std::string out;
    for (const auto& permission : json::Array(targets.at("permissions")))
      out += (out.empty() ? "" : ", ") + json::String(permission);
    return name + " gives Cards: " + out;
  }
  return name;
}

auto Decisions::Find(const std::string& conversation, const std::string& call,
                     const std::string& digest) -> std::optional<Decision> {
  auto q = db.prepare(
      "SELECT id,preview,digest FROM decisions WHERE conversation=? AND "
      "call=?");
  q.bind(1, conversation).bind(2, call);
  if (!q.step()) return std::nullopt;
  if (q.text(2) != digest)
    throw Failure("conflict",
                  "A decision cannot be used with different arguments");
  return Decision{q.text(0), q.text(1)};
}

auto Decisions::Request(const std::string& project,
                        const std::string& conversation,
                        const std::string& call, const std::string& digest,
                        const std::string& preview) -> Decision {
  if (auto old = Find(conversation, call, digest)) return *old;
  const auto id = crypto::uuid4();
  auto put = db.prepare(
      "INSERT INTO decisions(id,project,conversation,call,digest,preview,"
      "state) VALUES(?,?,?,?,?,?,'pending')");
  put.bind(1, id)
      .bind(2, project)
      .bind(3, conversation)
      .bind(4, call)
      .bind(5, digest)
      .bind(6, preview)
      .run();
  dependencies.event(
      conversation,
      json::Encode(json::Members{
          {"kind", json::Quote("decision")},
          {"id", json::Quote(id)},
          {"call_id", json::Quote(call)},
          {"asked", json::Encode(std::vector{json::Quote(
                        std::string(kDecisionMark) + preview)})}}));
  return {id, preview};
}

auto Decisions::Wait(std::unique_lock<std::recursive_mutex>& lock,
                     const std::string& id, bool wait, std::stop_token stop)
    -> void {
  changed.wait(lock, stop, [&] {
    if (stop.stop_requested() || stopping) throw Cancelled();
    auto query =
        db.prepare("SELECT state,preview FROM decisions WHERE id=?");
    query.bind(1, id);
    if (!query.step()) throw Failure("not_found", "Decision not found");
    const auto state = query.text(0);
    if (state == "denied")
      throw Failure("permission_denied", "The person declined this request");
    if (state == "allowed") return true;
    if (state != "pending")
      throw Failure(
          "outcome_unknown",
          "This decision was already used; consult the recorded outcome");
    if (!wait)
      throw Failure(
          "decision_required",
          "Waiting for the person's approval in Tsugumi: " +
              Summary(query.text(1)) +
              ". The request stays open in the Tsugumi app and Web UI, also "
              "when one opens later. Once the person has answered, call the "
              "same tool again with the same arguments.",
          id);
    return false;
  });
  if (stop.stop_requested()) throw Cancelled();
}

auto Decisions::Handle(const RequestIn& request, bool human) -> std::string {
  const auto* r = A::GetMessage(request.frame->data())->payload_as_Request();
  switch (r->value_type()) {
    case A::RequestPayload::DecisionResolveRequest: {
      Human(human);
      auto p = r->value_as_DecisionResolveRequest();
      const auto id = p->request_id()->str();
      auto q =
          db.prepare("SELECT conversation,state FROM decisions WHERE id=?");
      q.bind(1, id);
      if (!q.step()) throw Failure("not_found", "Decision not found");
      if (q.text(1) != "pending")
        throw Failure("conflict", "This decision was already answered");
      const auto conversation = q.text(0);
      const bool allow = p->allow();
      auto update = db.prepare("UPDATE decisions SET state=? WHERE id=?");
      update.bind(1, allow ? "allowed" : "denied").bind(2, id).run();
      changed.notify_all();
      dependencies.event(
          conversation,
          json::Encode(json::Members{
              {"kind", json::Quote("decisionAnswered")},
              {"id", json::Quote(id)},
              {"ok", allow ? "true" : "false"},
              {"text", json::Quote(allow ? "Approved this request"
                                         : "Declined; nothing changed")}}));
      S::AcknowledgementT result;
      result.accepted = allow;
      return wire::AgentReply(request.request_id, result);
    }
    case A::RequestPayload::DecisionListRequest: {
      const auto* filter = r->value_as_DecisionListRequest();
      const auto id = Text(filter->conversation_id());
      const auto request_id = Text(filter->request_id());
      if (!human && id.empty())
        throw Failure("permission_denied",
                      "External clients must name their conversation");
      if (!id.empty()) conversations->OwnConversation(id, human);
      A::DecisionListT result;
      size_t remaining = kPageBytes;
      flatbuffers::FlatBufferBuilder measure;
      // The cursor names the first row not yet returned.
      auto q = db.prepare(
          "SELECT rowid,id,project,conversation,preview,state,call "
          "FROM decisions WHERE (?1='' OR conversation=?1) "
          "AND (?2='' OR id=?2) AND (?3='' OR project=?3) "
          "AND (NOT ?4 OR state='pending') AND (?5=0 OR rowid<=?5) "
          "ORDER BY rowid DESC");
      q.bind(1, id)
          .bind(2, request_id)
          .bind(3, Text(filter->project_id()))
          .bind(4, filter->pending_only())
          .bind(5, filter->cursor());
      while (q.step()) {
        auto entry = std::make_unique<A::DecisionT>();
        entry->id = q.text(1);
        entry->project_id = q.text(2);
        entry->conversation_id = q.text(3);
        if (const auto root = registry.Location(entry->project_id))
          entry->project_path = root->string();
        entry->state = q.text(5);
        entry->call_id = q.text(6);
        entry->asked = {std::string(kDecisionMark) + q.text(4)};
        auto bytes = EntryBytes(measure, *entry);
        if (bytes > kPageBytes) {
          entry->state = "unavailable";
          entry->unavailable_reason = "The decision exceeds the RPC frame size";
          entry->asked.clear();
          bytes = EntryBytes(measure, *entry);
          if (bytes > kPageBytes)
            throw Failure("resource_exhausted",
                          "Decision metadata exceeds the RPC frame size");
        }
        if (bytes > remaining) {
          result.next_cursor = q.i64(0);
          break;
        }
        remaining -= bytes;
        result.decisions.push_back(std::move(entry));
      }
      return wire::AgentReply(request.request_id, std::move(result));
    }
    default:
      throw Failure("invalid_request", "Unsupported decision request");
  }
}
}  // namespace tsugumi::host
