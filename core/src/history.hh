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
#include <string>
#include <thread>
#include <vector>

#include "model.hh"
#include "util/sqlite.hh"

namespace tsugumi {

struct InvocationRecordData {
  InvocationSummaryData summary;
  std::vector<Value> inputs, outputs;  // only when record_io
  std::vector<InputSourceData> input_sources;
  std::optional<DisplayData> display;
  bool record_io = false;
};

struct StoredValueMeta {
  std::string name;
  PortKind kind;
  std::string bfbs;
  std::string root_type;
  uint64_t size_bytes = 0;
  std::optional<std::string> artifact_id;
};

struct LogEnd {
  std::string run_id;
  std::optional<CardPath> card;
  std::optional<uint64_t> invocation;
  LogStream stream;
  uint64_t end_offset;
};

struct LogRead {
  std::string text;
  uint64_t offset = 0, next_offset = 0, size = 0;
  bool exists = false, truncated = false;
};

class History {
 public:
  // `fault` (tests only): "history_invocation" makes every invocation record write fail.
  History(const std::filesystem::path& db_path, std::string cursor_secret, std::string fault = {});
  ~History();

  using Work = std::function<void(sql::Db&)>;
  // Ordered async write. Blocks while the queue is full (backpressure).
  void submit(Work work, std::function<void(bool ok)> done = {});
  // Synchronous write; throws Failed(persistence_failed) on error.
  void write_sync(const Work& work);
  // Consistent read transaction on a dedicated reader connection.
  template <class F>
  auto read(F&& f) -> decltype(f(std::declval<sql::Db&>()));

  // --- write helpers (call inside a Work) ---
  static void insert_run(sql::Db& db, const RunSummaryData& s, const std::string& op_id);
  static void update_run(sql::Db& db, const RunStatusData& s);
  static void put_status(sql::Db& db, const InvocationSummaryData& s);
  void complete_invocation(sql::Db& db, const InvocationRecordData& r) const;
  // Waits until every write submitted before this call has been committed or failed.
  void flush();
  static void put_last_input(sql::Db& db, const CardPath& card, const std::string& run_id,
                             uint64_t invocation, const std::vector<Value>& inputs);
  static void put_artifact(sql::Db& db, const ArtifactData& a);
  static void put_display(sql::Db& db, const DisplayData& d);
  static void put_receipt(sql::Db& db, const std::string& op_id, const std::string& kind,
                          const std::string& digest, const std::string& bytes);
  // Appends UTF-8 text to a log stream; returns the new end offsets per scope.
  std::vector<LogEnd> append_log(sql::Db& db, const std::string& run_id, const CardPath& card,
                                 uint64_t invocation, LogStream stream, const std::string& text);
  // Run level journal lines (no card).
  std::vector<LogEnd> append_run_journal(sql::Db& db, const std::string& run_id, const std::string& text);

  // --- reads ---
  static std::optional<RunSummaryData> run(sql::Db& db, const std::string& run_id);
  static std::optional<RunSummaryData> latest_run(sql::Db& db);
  static std::optional<std::string> run_receipt_digest(sql::Db& db, const std::string& op_id);
  struct ReceiptRow { std::string kind, digest, bytes; };
  static std::optional<ReceiptRow> receipt(sql::Db& db, const std::string& op_id);
  static std::vector<InvocationSummaryData> invocations(sql::Db& db, const std::string& run_id,
                                                        const CardPath& card, uint64_t after,
                                                        uint64_t limit);
  static std::optional<InvocationRecordData> invocation(sql::Db& db, const std::string& run_id,
                                                        const CardPath& card, uint64_t n);
  static std::optional<InvocationSummaryData> status(sql::Db& db, const std::string& run_id,
                                                     const CardPath& card, uint64_t n);
  static std::optional<uint64_t> latest_invocation(sql::Db& db, const std::string& run_id,
                                                   const CardPath& card, bool include_status);
  static std::optional<uint64_t> latest_completed(sql::Db& db, const std::string& run_id,
                                                  const CardPath& card);
  static bool run_has_card(sql::Db& db, const std::string& run_id, const CardPath& card);
  struct Aggregate { uint64_t calls = 0, succeeded = 0, failed = 0, cancelled = 0, total_ms = 0; };
  static Aggregate aggregate(sql::Db& db, const std::string& run_id, const CardPath& card);
  static std::optional<FailureData> latest_failure(sql::Db& db, const std::string& run_id,
                                                   const CardPath& card);
  static std::vector<StoredValueMeta> value_metas(sql::Db& db, const std::string& run_id,
                                                  const CardPath& card, uint64_t n, bool input);
  static std::optional<std::string> value_bytes(sql::Db& db, const std::string& run_id,
                                                const CardPath& card, uint64_t n, bool input,
                                                const std::string& port);
  struct LastInput {
    std::string run_id;
    uint64_t invocation;
    std::vector<Value> inputs;
  };
  static std::optional<LastInput> last_input(sql::Db& db, const CardPath& card);
  // Rewrites the stage times etc. of a completed invocation (the save stage
  // ends after its own write).
  static void update_detail(sql::Db& db, const InvocationSummaryData& s);
  // Latest saved value of one port, whichever invocation consumed it (an
  // `any` Card consumes one port per invocation).
  static void put_last_port_input(sql::Db& db, const CardPath& card, const std::string& run_id, uint64_t invocation,
                                  const Value& value);
  static std::optional<LastInput> last_port_input(sql::Db& db, const CardPath& card, const std::string& port);
  static std::optional<ArtifactData> artifact(sql::Db& db, const std::string& artifact_id);
  static std::vector<ArtifactData> artifacts(sql::Db& db, const std::string& run_id,
                                             const std::optional<CardPath>& card,
                                             std::optional<uint64_t> invocation,
                                             const std::string& after, uint64_t limit);
  static std::optional<DisplayData> display(sql::Db& db, const std::string& run_id, const CardPath& card);
  static std::vector<RunSummaryData> runs_page(sql::Db& db, std::optional<CardPath> card,
                                               bool artifacts_only, int64_t below_seq,
                                               int64_t max_seq, uint64_t limit,
                                               std::vector<int64_t>* seqs);
  static int64_t max_run_seq(sql::Db& db);
  static std::vector<std::string> unfinished_runs(sql::Db& db);
  LogRead read_log(sql::Db& db, const std::string& run_id, const std::optional<CardPath>& card,
                   std::optional<uint64_t> invocation, LogStream stream, uint64_t offset, bool tail,
                   uint64_t limit);

  // Cursors: signed, query-bound, expiring opaque strings.
  std::string make_cursor(const std::string& query_digest, int64_t hi, const std::string& after) const;
  // Returns {hi, after} or throws Failed(invalid_cursor).
  std::pair<int64_t, std::string> open_cursor(const std::string& cursor,
                                              const std::string& query_digest) const;

  std::string schema_blob(sql::Db& db, const std::string& hash);
  const std::filesystem::path& path() const { return path_; }

  // --- the project's copy of finished runs (.tsugumi/runs.sqlite) ---
  // Its tables are a fixed contract, read by others: runs, run_cards,
  // invocations, ivalues (FlatBuffers bytes, typed by `schemas`: bfbs by
  // hash), logs, artifacts and displays, plus meta(format, version). Runs
  // are matched by run id; only finished runs are copied, each once.
  // Adds the copy's runs missing here; returns how many.
  size_t import_runs(const std::filesystem::path& copy);
  // Adds the runs missing in the copy (creating it); returns how many.
  size_t export_runs(const std::filesystem::path& copy);
  // Deletes finished runs (all, or those that ended before `ended_before`)
  // here and in the copy when it exists; returns their run ids.
  std::vector<std::string> delete_runs(const std::filesystem::path& copy, std::optional<int64_t> ended_before);

 private:
  void writer_loop();
  void init_schema(sql::Db& db);
  sql::Db& reader();

  std::filesystem::path path_;
  std::mutex copy_mu_;  // one import, export or deletion at a time
  std::string secret_;
  std::string fault_;
  std::unique_ptr<sql::Db> writer_db_;
  std::mutex wmu_;
  std::condition_variable wcv_, wspace_;
  std::deque<std::pair<Work, std::function<void(bool)>>> wq_;
  bool stopping_ = false;
  std::thread writer_;
  std::mutex rmu_;
  std::map<std::thread::id, std::unique_ptr<sql::Db>> readers_;
  // Log offsets per scope key, maintained by the writer thread.
  std::map<std::string, uint64_t> log_ends_;
  // Progress of running invocations after their persisted start (stage
  // changes are not written; queries see them through overlay()).
  std::mutex live_mu_;
  std::map<std::string, InvocationSummaryData> live_;

 public:
  void set_live(const InvocationSummaryData& s);
  void clear_live(const InvocationSummaryData& s);
  // Replaces a persisted in-progress status with its current progress.
  void overlay(InvocationSummaryData& s);
};

template <class F>
auto History::read(F&& f) -> decltype(f(std::declval<sql::Db&>())) {
  sql::Db& db = reader();
  sql::Transaction t(db, false);
  return f(db);
}

}  // namespace tsugumi
