// Nix evaluation/build lane, generated Card SDK files and build cache.
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

#include "nix_eval.hh"
#include "graph.hh"
#include "model.hh"

namespace tsugumi {

class Service;
class NixEvaluator;

struct CardBuild {
  // The body (`run`): a package whose lib/main.so the worker loads, or a bash script.
  std::optional<std::string> package;
  std::optional<std::string> bash;
  std::string pre_run;
  std::string post_run;
  // The program the kernel runs the Card's binaries with (binfmt_misc), for a
  // Card built for another system; "" when it runs natively.
  std::string emulator;
  // The worker that loads a package of another system, built with the Card's
  // nixpkgs for that system; "" for tsugumid's own worker.
  std::string worker;
  std::string key;      // the manifest's store path
  bool cached = false;  // the store already had the result: nothing was built
};

struct BuildRequest {
  std::string run_id;
  const Node* node = nullptr;
  DocumentPtr doc;
  std::filesystem::path run_dir;
  uint32_t jobs = 1;
  uint32_t cores = 1;
  std::optional<uint64_t> timeout_seconds;
  const std::atomic<bool>* cancel = nullptr;
  std::function<void(const std::string&)> log;  // build output lines
  std::function<void(Phase)> on_phase;          // evaluate, then build (only when the store lacks the result)
};

struct CacheClearOutcome {
  std::vector<std::string> deleted;
  std::vector<std::string> retained;
  uint64_t freed_bytes = 0;
};

struct SDKJob {
  std::string op_id;
  CardPath card;
  uint64_t rev = 0;
  enum class State { Pending, Ready, Failed } state = State::Pending;
  std::vector<std::tuple<std::string, std::string, uint64_t>> files;  // path, hash, size
  std::optional<FailureData> failure;
};

class NixRunner {
 public:
  NixRunner(Service& svc, std::filesystem::path eval_dir);
  ~NixRunner();

  // _inputs.fbs/_outputs.fbs/_params.fbs (+ .bfbs) and JSON parameter files.
  static std::map<std::string, std::string> generate(const Node& node);
  // Builds JSON for all params (worker) and for nix:true params (Nix).
  static std::string params_json(const Node& node, bool nix_only);
  // FlatBuffer of all effective params (table Params).
  static std::string params_buffer(const Node& node);
  static types::TypeRef params_type(const Node& node);

  CardBuild build(const BuildRequest& req);

  // CardSDKPrepare / CardSDKStatus.
  SDKJob prepare_sdk(const std::string& op_id, const Node& node, uint64_t rev);
  std::optional<SDKJob> sdk_job(const std::string& op_id);
  bool sdk_jobs_active();
  // Bytes of a published SDK file (".sdk/<op_id>/<name>"). With `rev`, throws
  // revision_conflict when the job was prepared for another revision.
  std::optional<std::string> sdk_file(const std::string& path, std::optional<uint64_t> rev = std::nullopt);

  CacheClearOutcome clear_cache();
  // A flake to opt in to, as the settings ask for it: a flake reference or a GitHub page's URL.
  FlakeInfo resolve_flake(const std::string& ref);
  bool available() const { return true; }

 private:
  std::filesystem::path cache_dir() const;
  std::filesystem::path gcroots_dir() const;
  void acquire_lane(const std::atomic<bool>* cancel);
  void release_lane();
  // The nixpkgs pin of a pkgsPath (each Card uses its own project's).
  std::string nixpkgs_pin_json(const std::string& pkgs_path) const;
  NixEvaluator& evaluator();
  std::filesystem::path content_dir(const char* kind, const std::string& digest,
                                    const std::function<std::map<std::string, std::string>()>& files);
  void prune_roots(const std::string& prefix = {}, const std::string& keep = {});

  Service& svc_;
  std::filesystem::path eval_dir_;  // local folder: definitions and generated files by content
  std::filesystem::path sdk_dir_;
  std::mutex eval_mu_;
  std::unique_ptr<NixEvaluator> evaluator_;
  std::mutex dirs_mu_;
  std::set<std::string> written_;
  std::mutex lane_mu_;
  std::condition_variable lane_cv_;
  bool lane_busy_ = false;
  std::mutex sdk_mu_;
  std::map<std::string, SDKJob> sdk_jobs_;
  std::vector<std::thread> sdk_threads_;
};

}  // namespace tsugumi
