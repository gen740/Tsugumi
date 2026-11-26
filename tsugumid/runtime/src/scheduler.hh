// Run acceptance and execution. One run per document at a time; the run uses
// an immutable snapshot of the confirmed document.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>

#include "model.hh"
#include "server.hh"
#include "tsugumi/document/graph.hh"

namespace tsugumi {

namespace sql { class Db; }

class Service;
class RunExec;
struct RemoteJobRecord;
struct LogRead;

struct RunPlanData {
  std::vector<CardPath> targets;
  std::vector<CardPath> starts;
  std::vector<CardPath> build_candidates;
  // Subgraph runs: inner Cards fed from outside the subgraph. They receive
  // their saved values for exactly those ports (also listed in starts).
  std::vector<CardPath> seeds;
  // Partial runs: Cards further down that wait for all their inputs and have
  // ports nothing in this run feeds. They receive their saved values for
  // exactly those ports and run when the values of this run arrive.
  struct Join {
    CardPath card;
    std::set<std::string> saved_ports;
  };
  std::vector<Join> joins;
  std::vector<Diag> diagnostics;
};

struct RunParams {
  struct InjectedInput {
    CardPath card;
    Value value;
  };
  std::string op_id;
  std::string
      commit;  // the project's version it starts from ("" when not versioned)
  std::string digest;
  CardPath card;
  bool propagate = true;
  uint32_t max_concurrency = 4;
  std::optional<uint32_t> nix_jobs;
  std::optional<uint32_t> nix_cores;
  std::optional<std::vector<InjectedInput>> injected_inputs;
};

class Scheduler {
 public:
  explicit Scheduler(Service& svc);
  ~Scheduler();

  // Marks runs left unfinished by a previous process as failed/interrupted.
  void recover_interrupted();
  auto start_remote_cleanup() -> void;
  static RunPlanData plan(const Resolution& res, const CardPath& card,
                          bool propagate);
  /// Drafts and the Cards that need their outputs (they do not run).
  // Accepts a run (durable before returning) and starts it. Returns the
  // OperationReceipt bytes of the RunAccepted result.
  std::string accept(DocumentPtr doc, const RunParams& p);
  std::optional<RunStatusData> active_status();
  auto live_log(const std::string& run_id, const std::function<LogRead(sql::Db&)>& read) -> LogRead;
  RunStatusData cancel(const std::string& run_id);
  // Invalidate resident workers whose Card definitions changed.
  void document_changed(const Document& before, const Document& after);
  void shutdown();

  // Called by RunExec.
  auto finished(RunSummaryData sum, std::string record) -> void;

 private:
  Service& svc_;
  // Admission and terminal publication share this lock. History writer
  // callbacks must not acquire it.
  std::mutex mu_;
  std::condition_variable finished_;
  std::shared_ptr<RunExec> active_;
  struct Completion {
    std::shared_ptr<RunExec> run;
    RunStatusData status;
    std::string record;
  };
  auto reap_completed() -> void;
  std::deque<Completion> completions_;
  std::atomic<bool> stopping_{false};
  std::thread remote_cleanup_;
  std::thread reaper_;
};

}  // namespace tsugumi
