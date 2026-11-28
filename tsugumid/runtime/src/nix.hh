// Card evaluation and builds, and the build cache.
#pragma once

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "model.hh"
#include "nix_eval.hh"
#include "tsugumi/document/graph.hh"
#include "tsugumi/protocol/runtime_state_generated.h"
#include "tsugumi/workerd/card_plan.hh"

namespace tsugumi {

class Service;
class NixEvaluator;

struct CardBuild {
  std::string package;  // store path containing lib/main.so
  std::vector<schema::CardView> views;
  // The program the kernel runs the Card's binaries with (binfmt_misc), for a
  // Card built for another system; "" when it runs natively.
  std::string emulator;
  // The worker that loads a package of another system, built with the Card's
  // nixpkgs for that system; "" for tsugumid's own worker.
  std::string worker;
  WorkerDerivation derivation;
  BuildEnvironmentData environment;
  std::string key;  // the build plan's store path
  uint64_t preparation_ms =
      0;                // evaluation/build time, excluding closure transport
  bool card_probe = false;
  bool cached = false;  // the store already had the result: nothing was built
  std::string substituter;
  uint32_t implementation_index = 1;
  std::string implementation_label;  // "" when its run list entry has none
};

struct CardBuildPlan {
  CardBuild common;
  // What identifies the evaluation (pin, system, arguments), for builds
  // prepared ahead of their runs.
  std::string evaluation_key;
  std::vector<std::string> implementations;
  std::vector<std::string> implementation_labels;  // one per implementation
  bool execution_list = false;
  auto implementation(size_t index) const -> CardBuild;
};

struct BuildRequest {
  const Node* node = nullptr;
  DocumentPtr doc;
  std::optional<uint32_t> jobs;
  std::optional<uint32_t> cores;
  uint32_t max_implementations = kMaxExecutionBatchSize;
  std::optional<uint64_t> timeout_seconds;
  ExecutionTarget target;
  const DerivationClosureTransport* transport = nullptr;
  bool probe_only = false;
  // Called after evaluation so a cache hit can skip transferring and building.
  std::function<bool(std::string_view output)> needs_probe_build;
  // Called during a probe evaluation once the Card is known to declare a
  // probe, before the probe itself is evaluated.
  std::function<void()> on_probe_declared;
  std::stop_token cancel;
  NixOutput log;  // build output
  std::function<void(Phase)>
      on_phase;  // evaluate, then build (only when the store lacks the result)
  std::function<void(uint32_t)> on_plan;  // implementation count before build
  std::function<void(const std::string& derivation, bool active)>
      on_build;  // a derivation build starting or ending
};

struct ExpressionRequest {
  DocumentPtr doc;
  CardPath card;
  std::string expression;
  bool build = false;
  std::stop_token cancel;
  std::optional<std::chrono::steady_clock::time_point> deadline;
  NixOutput log;
};

struct CacheClearOutcome {
  std::vector<std::string> deleted;
  std::vector<std::string> retained;
  std::optional<uint64_t> freed_bytes = 0;
};

class NixRunner {
 public:
  explicit NixRunner(Service& svc);
  ~NixRunner();

  auto build(const BuildRequest& req) -> CardBuildPlan;
  // Build only the declared view files on the UI's native store.
  auto declared_views(const BuildRequest& req) -> std::vector<schema::CardView>;
  static auto execution_systems() -> std::vector<std::string>;
  auto evaluate_expression(const ExpressionRequest& req) -> ExpressionResult;
  auto lock_source(const std::string& url) -> NixSource;

  CacheClearOutcome clear_cache();

 private:
  auto build_plan(const BuildRequest& req, bool views_only) -> CardBuildPlan;
  auto configure_call(NixCall& call, const Document& doc, const Node& node,
                      bool expression_only = false) -> void;
  std::filesystem::path cache_dir() const;
  std::filesystem::path gcroots_dir() const;
  NixEvaluator& evaluator();
  // The prepared build of `card` (--prepared-cards), checked against the
  // evaluation the core would run now.
  auto prepared(const std::string& card, const std::string& key,
                uint32_t max_implementations) const -> NixResult;

  Service& svc_;
  std::filesystem::path nix_dir_;
  std::mutex eval_mu_;
  std::unique_ptr<NixEvaluator> evaluator_;
  // Entries are per Card and system, capped at 256.
  std::mutex evaluations_mu_;
  std::map<std::tuple<std::string, std::string, bool>,
           std::pair<EvaluationKey, NixResult>>
      evaluations_;
  std::optional<std::map<std::string, Tsugumi::State::PreparedCardT>> prepared_;
  uint64_t full_evaluations_ = 0;
};

}  // namespace tsugumi
