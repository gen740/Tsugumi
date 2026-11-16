#include <ctime>
#include <limits>

#include "store.hh"
#include "util/crypto.hh"

namespace tsugumi {

auto Store::cursor() const -> uint64_t {
  std::lock_guard lock(mu_);
  return cursor_;
}

auto Store::lane_record(const std::string& id, const WriterLane& lane) const
    -> schema::JournalLane {
  schema::JournalLane record;
  *record.id = id;
  *record.acknowledged = lane.acknowledged;
  *record.activity = lane.activity;
  *record.retired = false;
  *record.op = lane.outcome ? lane.outcome->op_id : "";
  *record.kind = lane.outcome ? lane.outcome->kind : "";
  *record.digest = lane.outcome ? lane.outcome->digest : lane.digest;
  *record.receipt_b64 = lane.outcome ? crypto::base64(lane.outcome->bytes) : "";
  *record.cursor = lane.outcome ? lane.outcome->cursor : 0;
  return record;
}

auto Store::restore_lane(const schema::JournalLane& record) -> void {
  if (*record.retired) {
    writers_.erase(*record.id);
    return;
  }
  WriterLane lane;
  lane.acknowledged = *record.acknowledged;
  lane.activity = *record.activity;
  lane.digest = *record.digest;
  if (!record.op->empty()) {
    Receipt outcome;
    outcome.op_id = *record.op;
    outcome.kind = *record.kind;
    outcome.digest = *record.digest;
    crypto::base64_decode(*record.receipt_b64, outcome.bytes);
    outcome.identity = OperationIdentity{*record.id, lane.acknowledged + 1};
    outcome.cursor = *record.cursor;
    lane.outcome = std::move(outcome);
  }
  writers_[*record.id] = std::move(lane);
}

auto Store::operation(const OperationIdentity& identity,
                      const std::string& digest, int64_t now) const
    -> std::optional<Receipt> {
  if (!crypto::is_uuid(identity.lane) || identity.sequence == 0)
    throw Failed(FailureKind::invalid_request,
                 "operation identity requires a server-issued lane and "
                 "positive sequence");
  std::lock_guard lock(mu_);
  const auto found = writers_.find(identity.lane);
  if (found == writers_.end() ||
      now - found->second.activity >= kWriterLeaseSeconds)
    throw Failed(FailureKind::operation_retired,
                 "writer lane is unknown or expired; inspect the previous "
                 "outcome before opening a new lane");
  const auto& lane = found->second;
  if (identity.sequence <= lane.acknowledged)
    throw Failed(FailureKind::operation_retired,
                 "operation was acknowledged and cannot execute again");
  if (identity.sequence != lane.acknowledged + 1)
    throw Failed(FailureKind::operation_conflict,
                 "acknowledge the preceding operation before submitting the "
                 "next sequence");
  if (!lane.digest.empty() && lane.digest != digest)
    throw Failed(
        FailureKind::operation_conflict,
        "operation identity was used with a different semantic payload");
  return lane.outcome;
}

auto Store::begin_operation(const OperationIdentity& identity,
                            const std::string& digest, int64_t now) -> void {
  (void)operation(identity, digest, now);
  if (digest.size() != 64 ||
      digest.find_first_not_of("0123456789abcdef") != std::string::npos)
    throw Failed(FailureKind::invalid_request,
                 "semantic digest must be SHA-256 hex");
  auto lane = writers_.at(identity.lane);
  if (!lane.digest.empty()) return;
  lane.digest = digest;
  lane.activity = now;
  persist_lane(lane_record(identity.lane, lane));
}

auto Store::operation_pending(const OperationIdentity& identity) const -> bool {
  std::lock_guard lock(mu_);
  const auto found = writers_.find(identity.lane);
  return found != writers_.end() && !found->second.digest.empty() &&
         !found->second.outcome;
}

auto Store::terminal_lane(const Receipt& receipt) const -> schema::JournalLane {
  const auto& identity = *receipt.identity;
  if (operation(identity, receipt.digest, std::time(nullptr)))
    throw Failed(FailureKind::operation_conflict,
                 "operation already has a terminal outcome");
  const size_t bytes = receipt.bytes.size() + receipt.op_id.size() +
                       receipt.kind.size() + receipt.digest.size();
  if (bytes > kMaxOutcomeBytes)
    throw Failed(FailureKind::resource_limit,
                 "operation outcome exceeds the 16 KiB writer-lane budget");
  auto lane = writers_.at(identity.lane);
  lane.activity = std::time(nullptr);
  lane.digest = receipt.digest;
  lane.outcome = receipt;
  return lane_record(identity.lane, lane);
}

auto Store::open_writer(int64_t now) -> std::string {
  expire_writers(now);
  if (writers_.size() >= kMaxWriterLanes)
    throw Failed(FailureKind::resource_limit,
                 "project already has 64 live writer lanes; acknowledge or "
                 "wait for an inactive lane to expire");
  const auto id = crypto::uuid4();
  WriterLane lane;
  lane.activity = now;
  persist_lane(lane_record(id, lane));
  return id;
}

auto Store::acknowledge(const OperationIdentity& identity,
                        uint64_t incorporated_cursor, int64_t now) -> void {
  // A duplicate acknowledgement is harmless; it never allocates a lane.
  const auto found = writers_.find(identity.lane);
  if (found == writers_.end() ||
      now - found->second.activity >= kWriterLeaseSeconds)
    throw Failed(FailureKind::operation_retired,
                 "writer lane is unknown or expired");
  auto lane = found->second;
  if (identity.sequence == 0 || identity.sequence > lane.acknowledged + 1)
    throw Failed(FailureKind::operation_conflict,
                 "acknowledgement is ahead of this writer");
  if (identity.sequence <= lane.acknowledged) return;
  if (!lane.outcome || incorporated_cursor < lane.outcome->cursor)
    throw Failed(
        FailureKind::operation_conflict,
        "incorporate the terminal outcome's cursor before acknowledging it");
  if (identity.sequence == std::numeric_limits<uint64_t>::max())
    throw Failed(FailureKind::resource_limit,
                 "writer sequence exhausted; open a new lane");
  lane.acknowledged = identity.sequence;
  lane.digest.clear();
  lane.outcome.reset();
  lane.activity = now;
  persist_lane(lane_record(identity.lane, lane));
}

auto Store::retire_writer(const std::string& id, int64_t now) -> void {
  const auto found = writers_.find(id);
  if (found == writers_.end()) return;
  if (found->second.outcome || !found->second.digest.empty())
    throw Failed(
        FailureKind::operation_conflict,
        "acknowledge the outstanding outcome before retiring the lane");
  auto record = lane_record(id, found->second);
  *record.retired = true;
  *record.activity = now;
  persist_lane(record);
}

auto Store::expire_writers(int64_t now) -> void {
  std::vector<schema::JournalLane> expired;
  for (const auto& [id, lane] : writers_) {
    if (now - lane.activity < kWriterLeaseSeconds) continue;
    auto record = lane_record(id, lane);
    *record.retired = true;
    expired.push_back(std::move(record));
  }
  for (const auto& record : expired) persist_lane(record);
}

}  // namespace tsugumi
