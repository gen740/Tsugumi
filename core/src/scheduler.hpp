// Run acceptance and execution. One run per document at a time; the run uses
// an immutable snapshot of the confirmed document.
#pragma once

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "graph.hpp"
#include "model.hpp"
#include "server.hpp"

namespace tsugumi {

class Service;
class RunExec;

struct RunPlanData {
  std::vector<CardPath> targets, starts, build_candidates;
  std::vector<Diag> diagnostics;
};

struct RunParams {
  std::string op_id;
  std::string digest;
  CardPath card;
  bool propagate = true;
  uint32_t max_concurrency = 4;
  uint32_t nix_jobs = 1;
  uint32_t nix_cores = 1;
};

class Scheduler {
 public:
  explicit Scheduler(Service& svc);
  ~Scheduler();

  // Marks runs left unfinished by a previous process as failed/interrupted.
  void recover_interrupted();
  static RunPlanData plan(const Resolution& res, const CardPath& card, bool propagate);
  // Accepts a run (durable before returning) and starts it. Returns the
  // OperationReceipt bytes of the RunAccepted result.
  std::string accept(DocumentPtr doc, const RunParams& p);
  std::optional<RunStatusData> active_status();
  RunStatusData cancel(const std::string& run_id);
  std::optional<Note> latest_display_note(const std::string& card_key);
  void shutdown();

  // Called by RunExec.
  void publish_display(const DisplayData& d);
  void finished(RunExec* exec);

 private:
  Service& svc_;
  std::mutex mu_;
  std::shared_ptr<RunExec> active_;
  std::vector<std::thread> reapers_;
  std::map<std::string, DisplayData> displays_;  // latest per card key (any run)
};

Note display_note(const DisplayData& d);

}  // namespace tsugumi
