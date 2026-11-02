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

#include "graph.hh"
#include "model.hh"

namespace tsugumi {

class Service;

struct CardBuild {
  std::optional<std::string> package;  // store path of the Card package
  std::optional<std::string> library;  // shared object to run (`run`; default package/lib/main.so)
  std::string pre_script, script, post_script;
  bool reuse_worker = true;  // the default: one loaded worker serves a Card for the whole run
  std::string key;
  bool cached = false;
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
  std::function<void(Phase)> on_phase;          // evaluate, then build (not called for cached builds)
};

struct CacheClearOutcome {
  std::vector<std::string> deleted, retained;
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
  explicit NixRunner(Service& svc);
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
  bool available() const { return !nix_build_.empty(); }

 private:
  std::filesystem::path cache_dir() const;
  std::filesystem::path gcroots_dir() const;
  void acquire_lane(const std::atomic<bool>* cancel);
  void release_lane();
  std::string nixpkgs_pin_json(const Document& doc) const;
  std::vector<std::string> nix_env() const;

  Service& svc_;
  std::string nix_build_, nix_store_, nix_instantiate_;
  std::string sdk_hash_;  // content hash of the installed SDK (part of every build key)
  std::mutex lane_mu_;
  std::condition_variable lane_cv_;
  bool lane_busy_ = false;
  std::mutex defs_mu_;
  std::set<std::string> copied_defs_;  // run_dir/def_id
  std::mutex sdk_mu_;
  std::map<std::string, SDKJob> sdk_jobs_;
  std::vector<std::thread> sdk_threads_;
};

}  // namespace tsugumi
