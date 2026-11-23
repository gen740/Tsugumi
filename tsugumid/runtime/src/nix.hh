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
  bool cached = false;  // the store already had the result: nothing was built
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
  std::stop_token cancel;
  NixOutput log;  // build output
  std::function<void(Phase)>
      on_phase;  // evaluate, then build (only when the store lacks the result)
  std::function<void(uint32_t)> on_plan;  // implementation count before build
  std::function<void(const std::string& derivation, bool active)>
      on_build;  // a derivation build starting or ending
};

// NIXPATH-CONTRACT: Replace this declaration with lane A's evaluator interface.
struct ExpressionRequest {
  DocumentPtr doc;
  CardPath card;
  std::string expression;
  bool build = false;
  std::string system;
  std::stop_token cancel;
  std::optional<std::chrono::steady_clock::time_point> deadline;
  NixOutput log;
};

struct CacheClearOutcome {
  std::vector<std::string> deleted;
  std::vector<std::string> retained;
  uint64_t freed_bytes = 0;
};

class NixRunner {
 public:
  NixRunner(Service& svc, std::filesystem::path eval_dir);
  ~NixRunner();

  // The port schemas given to a Card's builder: _inputs.fbs, _outputs.fbs
  // and _params.fbs.
  static std::map<std::string, std::string> generate(const Node& node);
  // Builds JSON for all effective params and for nix:true params (Nix).
  static std::string params_json(const Node& node, bool nix_only);
  static std::string params_json(const std::vector<ParamDef>& params,
                                 bool nix_only);
  // FlatBuffer of all effective params (table Params).
  static std::string params_buffer(const Node& node);
  static types::TypeRef params_type(const Node& node);

  auto build(const BuildRequest& req) -> CardBuildPlan;
  auto evaluate_expression(const ExpressionRequest& req) -> ExpressionResult;
  // Build only the declared view files on the UI's native store.
  auto declared_views(const BuildRequest& req) -> std::vector<schema::CardView>;
  static auto execution_systems() -> std::vector<std::string>;

  CacheClearOutcome clear_cache();
  // A flake to opt in to, as the settings ask for it: a flake reference or a
  // GitHub page's URL.
  FlakeInfo resolve_flake(const std::string& ref);

 private:
  auto build_plan(const BuildRequest& req, bool views_only) -> CardBuildPlan;
  std::filesystem::path cache_dir() const;
  std::filesystem::path gcroots_dir() const;
  // The nixpkgs pin of a pkgsPath (each Card uses its own project's).
  NixEvaluator& evaluator();
  // The prepared build of `card` (--prepared-cards), checked against the
  // evaluation the core would run now.
  auto prepared(const std::string& card, const std::string& key,
                uint32_t max_implementations) const -> NixResult;
  std::filesystem::path content_dir(
      const char* kind, const std::string& digest,
      const std::function<std::map<std::string, std::string>()>& files);

  Service& svc_;
  std::filesystem::path
      eval_dir_;  // local folder: definitions and generated files by content
  std::filesystem::path nix_dir_;
  std::mutex eval_mu_;
  std::unique_ptr<NixEvaluator> evaluator_;
  // At most one evaluation per Card, capped at 256 Cards; edits replace
  // entries, and deleted Cards are pruned.
  std::mutex evaluations_mu_;
  std::map<std::string, std::pair<EvaluationKey, NixResult>> evaluations_;
  std::optional<std::map<std::string, Tsugumi::State::PreparedCardT>>
      prepared_;
  uint64_t full_evaluations_ = 0;
  std::mutex dirs_mu_;
  std::set<std::string> written_;
};

}  // namespace tsugumi
