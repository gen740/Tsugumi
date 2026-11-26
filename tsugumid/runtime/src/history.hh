// SQLite is the source of truth for runs, invocations, progress status, logs,
// artifacts references and last saved inputs. Writes go through one ordered,
// bounded writer thread; callbacks fire only after the transaction commits.
#pragma once

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "model.hh"
#include "tsugumi/protocol/tsugumi_generated.h"
#include "tsugumi/util/sqlite.hh"

namespace tsugumi {

struct InvocationRecordData {
  InvocationSummaryData summary;
  std::vector<Value> inputs;   // only when record_io
  std::vector<Value> outputs;  // only when record_io
  std::vector<InputSourceData> input_sources;
  bool record_io = false;
};

struct RemoteJobRecord {
  InvocationSummaryData invocation;
  std::string instance;
  std::string spec;
  bool accepted = false;
  bool cancel_requested = false;
};

struct StoredValueMeta {
  std::string name;
  std::string bfbs;
  std::string root_type;
  uint64_t size_bytes = 0;
  struct Artifact {
    ArtifactKey id;
    std::optional<uint64_t> size;
    std::string label;
    Tsugumi::Data::ArtifactState state;
  };
  std::vector<Artifact> artifacts;
  std::string hash;  // SHA-256 of the recorded bytes

};

struct LogEnd {
  std::string run_id;
  std::optional<CardPath> card;
  std::optional<uint64_t> invocation;
  LogStream stream;
  std::optional<uint64_t> end_offset;
  bool live = false;
};

struct LogRead {
  std::string text;
  uint64_t offset = 0;
  uint64_t next_offset = 0;
  uint64_t size = 0;
  uint64_t line_number = 1;
  bool starts_line = true;
  bool exists = false;
  bool truncated = false;
  std::optional<uint64_t> raw_size;
  bool reset = false;
  std::optional<uint64_t> log_truncated_at_bytes;
};

class History {
 public:
  // `fault` (tests only): "history_invocation" makes every invocation record
  // write fail.
  History(const std::filesystem::path& db_path, std::string fault = {},
          const std::filesystem::path& run_scratch = {});
  ~History();
  using Work = std::function<void(sql::Db&)>;
  // Ordered async write. Blocks while the queue is full (backpressure).
  void submit(Work work, std::function<void(bool ok)> done = {});
  // Synchronous write; throws Failed(persistence_failed) on error.
  void write_sync(const Work& work);
  auto allocate_invocation_sequence(const InvocationSummaryData& invocation)
      -> uint64_t;
  // Consistent read transaction on a dedicated reader connection.
  template <class F>
  auto read(F&& f) -> decltype(f(std::declval<sql::Db&>()));

  // --- write helpers (call inside a Work) ---
  static auto put_environment(sql::Db& db, const std::string& run_id,
                              const BuildEnvironmentData& environment)
      -> std::string;
  // The binary caches configured while the run built its environments. A
  // run's first list stays (the daemon's settings are fixed for its life).
  static auto put_substituters(sql::Db& db, const std::string& run_id,
                               const std::vector<std::string>& urls) -> void;
  static auto next_run_id(sql::Db& db) -> std::string;
  static void insert_run(sql::Db& db, const RunSummaryData& s,
                         const std::string& op_id);
  static void update_run(sql::Db& db, const RunStatusData& s);
  static void put_status(sql::Db& db, const InvocationSummaryData& s);
  static auto put_batch(sql::Db& db, const RunBatchData& batch) -> void;
  static auto interrupt_batches(sql::Db& db, const std::string& run_id) -> void;
  static auto put_remote_job(sql::Db& db, const RemoteJobRecord& job) -> void;
  static auto has_remote_jobs(sql::Db& db) -> bool;
  void complete_invocation(sql::Db& db, const InvocationRecordData& r) const;
  // Waits until every write submitted before this call has been committed or
  // failed.
  void flush();
  static auto put_last_input(sql::Db& db, const CardPath& card,
                             const std::string& run_id, uint64_t activation,
                             const std::vector<SavedInputGroup>& groups)
      -> void;
  static void put_artifact(sql::Db& db, const ArtifactData& a);
  // Inspect a packed output using its export-validated reflection schema.
  // Debug calls verify the entire value separately before reading it.
  static auto output_artifacts(std::span<const Value> values)
      -> std::vector<OutputArtifactData>;
  static auto artifact_outputs(sql::Db& db, const ArtifactData& artifact)
      -> std::vector<std::pair<std::string, std::string>>;
  static auto output_artifact_ids(const types::Type& type, std::string_view bytes)
      -> std::set<ArtifactKey>;
  static void put_receipt(sql::Db& db, const std::string& op_id,
                          const std::string& kind, const std::string& digest,
                          const std::string& bytes);
  static auto open_live_logs() -> std::unique_ptr<sql::Db>;
  // Appends UTF-8 text to a log stream; returns the new end offsets per scope.
  static std::vector<LogEnd> append_log(sql::Db& db, const std::string& run_id,
                                 const CardPath& card, uint64_t invocation,
                                 LogStream stream, const std::string& text);
  static auto mark_log_truncated(sql::Db& db, const std::string& run_id,
                                 const CardPath& card, uint64_t invocation,
                                 uint64_t limit_bytes, bool build = false) -> void;
  // Run level journal lines (no card).

  // --- reads ---
  static std::optional<RunSummaryData> run(sql::Db& db,
                                           const std::string& run_id);
  static std::optional<RunSummaryData> latest_run(sql::Db& db);
  struct ReceiptRow {
    std::string kind;
    std::string digest;
    std::string bytes;
  };
  static std::optional<ReceiptRow> receipt(sql::Db& db,
                                           const std::string& op_id);
  // Each Card's latest invocation of a run, completed or in progress.
  static std::vector<InvocationSummaryData> latest_invocations(
      sql::Db& db, const std::string& run_id);
  // The latest activation for each Card, including a batch with no invocations.
  static auto latest_batches(sql::Db& db, const std::string& run_id,
                             const std::optional<CardPath>& card = std::nullopt)
      -> std::vector<RunBatchData>;
  static std::vector<InvocationSummaryData> invocations(
      sql::Db& db, const std::string& run_id, const CardPath& card,
      uint64_t after, uint64_t limit);
  static std::optional<InvocationRecordData> invocation(
      sql::Db& db, const std::string& run_id, const CardPath& card, uint64_t n);
  static std::optional<InvocationSummaryData> status(sql::Db& db,
                                                     const std::string& run_id,
                                                     const CardPath& card,
                                                     uint64_t n);
  static std::optional<uint64_t> latest_invocation(sql::Db& db,
                                                   const std::string& run_id,
                                                   const CardPath& card,
                                                   bool include_status);
  static std::optional<uint64_t> latest_completed(sql::Db& db,
                                                  const std::string& run_id,
                                                  const CardPath& card);
  static bool run_has_card(sql::Db& db, const std::string& run_id,
                           const CardPath& card);
  struct Aggregate {
    uint64_t calls = 0;
    uint64_t succeeded = 0;
    uint64_t failed = 0;
    uint64_t cancelled = 0;
    uint64_t total_ms = 0;
  };
  static Aggregate aggregate(sql::Db& db, const std::string& run_id,
                             const CardPath& card);
  static std::optional<FailureData> latest_failure(sql::Db& db,
                                                   const std::string& run_id,
                                                   const CardPath& card);
  static std::vector<StoredValueMeta> value_metas(sql::Db& db,
                                                  const std::string& run_id,
                                                  const CardPath& card,
                                                  uint64_t n, bool input);
  static std::optional<std::string> value_bytes(sql::Db& db,
                                                const std::string& run_id,
                                                const CardPath& card,
                                                uint64_t n, bool input,
                                                const std::string& port);
  // A page of a stored value: its bytes from `offset` (at most `limit`), with
  // the value's size and SHA-256 (stored with it, so nothing else is read).
  struct ValuePage {
    std::string bytes;
    uint64_t size = 0;
    std::string hash;
  };
  static std::optional<ValuePage> value_page(
      sql::Db& db, const std::string& run_id, const CardPath& card, uint64_t n,
      bool input, const std::string& port, uint64_t offset, uint64_t limit);
  struct LastInput {
    std::string run_id;
    uint64_t invocation;  // Activation that saved the groups.
    std::vector<SavedInputGroup> groups;
  };
  static std::optional<LastInput> last_input(sql::Db& db, const CardPath& card);
  // Rewrites the stage times etc. of a completed invocation (the save stage
  // ends after its own write).
  static void update_detail(sql::Db& db, const InvocationSummaryData& s);
  // Latest saved group of one port, whichever activation consumed it (an
  // `any` Card consumes one port per activation).
  static auto put_last_port_input(sql::Db& db, const CardPath& card,
                                  const std::string& run_id,
                                  uint64_t activation,
                                  const SavedInputGroup& group) -> void;
  static std::optional<LastInput> last_port_input(sql::Db& db,
                                                  const CardPath& card,
                                                  const std::string& port);
  static std::optional<ArtifactData> artifact(sql::Db& db,
                                              const std::string& artifact_id);
  static auto transient_artifacts(sql::Db& db, const std::string& run_id)
      -> std::vector<ArtifactData>;
  static std::vector<ArtifactData> artifacts(
      sql::Db& db, const std::string& run_id,
      const std::optional<CardPath>& card, std::optional<uint64_t> invocation,
      const std::string& after, uint64_t limit);
  static std::vector<RunSummaryData> runs_page(sql::Db& db,
                                               std::optional<CardPath> card,
                                               bool artifacts_only,
                                               int64_t below_seq,
                                               int64_t max_seq, uint64_t limit,
                                               std::vector<int64_t>* seqs);
  static int64_t max_run_seq(sql::Db& db);
  static std::vector<std::string> unfinished_runs(sql::Db& db);
  static auto read_log(sql::Db& db, const std::string& run_id,
                       const std::optional<CardPath>& card,
                       std::optional<uint64_t> invocation, LogStream stream,
                       uint64_t offset, bool tail, uint64_t limit,
                       std::optional<uint64_t> before, bool terminal,
                       std::optional<uint64_t> since) -> LogRead;

  // Cursors carry a paging position.
  std::string make_cursor(int64_t hi, const std::string& after) const;
  // Returns {hi, after} or throws Failed(invalid_cursor).
  std::pair<int64_t, std::string> open_cursor(const std::string& cursor) const;

  const std::filesystem::path& path() const { return path_; }

  static auto delete_category(sql::Db& db, const std::string& run_id,
                              Tsugumi::RPC::RunRecordCategory category) -> void;

  // Deletes the selected finished runs from .tsugumi/runs.sqlite.
  std::vector<std::string> delete_runs(
      std::optional<int64_t> ended_before,
      const std::optional<std::vector<std::string>>& run_ids);

 private:
  struct ReaderLease {
    History* owner = nullptr;
    std::unique_ptr<sql::Db> db;
    ~ReaderLease() {
      if (owner && db) owner->release_reader(std::move(db));
    }
    sql::Db& operator*() const { return *db; }
  };
  void writer_loop();
  void init_schema(sql::Db& db);
  ReaderLease acquire_reader();
  void release_reader(std::unique_ptr<sql::Db> db);

  std::filesystem::path path_;
  std::string fault_;
  std::unique_ptr<sql::Db> writer_db_;
  std::mutex wmu_;
  std::condition_variable wcv_;
  std::condition_variable wspace_;
  std::deque<std::pair<Work, std::function<void(bool)>>> wq_;
  bool stopping_ = false;
  std::thread writer_;
  std::mutex rmu_;
  std::vector<std::unique_ptr<sql::Db>> reader_pool_;
  struct LogPosition {
    uint64_t offset = 0;
    uint64_t newlines = 0;
  };
  // Progress of running invocations after their persisted start (stage
  // changes are not written; queries see them through overlay()).
  std::mutex live_mu_;
  std::map<std::string, InvocationSummaryData> live_;

 public:
  void set_live(const InvocationSummaryData& s);
  void clear_live(const InvocationSummaryData& s);
  // The progress of a run's running invocations.
  std::vector<InvocationSummaryData> live(const std::string& run_id);
  // Replaces a persisted in-progress status with its current progress.
  void overlay(InvocationSummaryData& s);
};

template <class F>
auto History::read(F&& f) -> decltype(f(std::declval<sql::Db&>())) {
  auto lease = acquire_reader();
  sql::Transaction t(*lease, false);
  return f(*lease);
}

}  // namespace tsugumi
