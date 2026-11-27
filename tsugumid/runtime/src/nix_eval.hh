// Cached libnix evaluation runs in a child that Stop can terminate. Transfers
// use their own store connections in the core. Native builds use a separate
// Nix client so each connection has its own build options.
#pragma once

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "tsugumi/util/process.hh"
#include "tsugumi/workerd/derivation.hh"

namespace tsugumi {

inline constexpr uint32_t kMaxExecutionBatchSize = 256;

struct NixSource {
  std::string url, rev, nar_hash;
  auto operator<=>(const NixSource&) const = default;
};

using LockedNixPath = std::map<std::string, NixSource>;

struct NixDefinitionScope {
  std::string directory;
  std::string card;
  std::vector<std::string> files = {};
  std::string generated_inputs = {};
  std::string digest = {};
  auto operator<=>(const NixDefinitionScope&) const = default;
};

using EvaluationKey = std::tuple<std::string, std::string>;

using NixOutput = std::function<void(proc::LineChunk)>;

struct NixCall {
  NixSource core_nixpkgs;  // only consumed by the trusted plan builder
  std::filesystem::path nix_dir;
  std::string args;
  // Confirmed definition sources, also available to alternative build backends
  // without parsing the Nix expression above.
  std::filesystem::path definition_dir;
  std::string definition_key;  // content, project lock and build arguments
  std::string card;
  std::vector<NixDefinitionScope> definitions;
  LockedNixPath locked_nix_path;
  std::string system;
  bool needs_worker = false;
  // GC root created for the result: <root_dir>/<root_prefix><result name>, a
  // symlink (registered as an indirect root outside /nix/var/nix/gcroots).
  std::filesystem::path root_dir;
  std::string root_prefix;
  std::optional<uint32_t> jobs;
  std::optional<uint32_t> cores;
  bool views_only = false;
  bool probe_only = false;
  uint32_t max_implementations = kMaxExecutionBatchSize;
  std::stop_token cancel;
  std::optional<std::chrono::steady_clock::time_point> deadline;
  NixOutput log;  // evaluation messages and build output
  std::function<void(bool building)>
      on_phase;  // false: evaluating, true: building
  std::function<void(const std::string& derivation, bool active)>
      on_build;  // a derivation build starting or ending
  auto evaluation_key() const -> EvaluationKey {
    return {system, definition_key.empty() ? args : definition_key};
  }
};

struct NixResult {
  WorkerDerivation derivation;
  std::string plan_json;
  std::vector<std::string> closure;
  bool built = false;
  std::string substituter;
  bool execution_list = false;
  uint32_t implementation_count = 1;
  // One per implementation, in run list order; "" for an unlabeled one.
  std::vector<std::string> implementation_labels;
  uint64_t evaluation_epoch = 0;
};

struct ExpressionFile {
  std::string path, bytes;
};

struct ExpressionResult {
  std::optional<std::string> json;
  std::vector<ExpressionFile> files;
  bool truncated = false;
  std::string log_tail;
  std::optional<std::string> build_error;
};

inline auto sandboxed_daemon_build_allowed(bool trusted_client,
                                           std::string_view guest_sandbox)
    -> bool {
  return trusted_client || guest_sandbox == "1";
}

class NixEvaluator {
 public:
  // Must run on the main thread before other threads start (Boehm GC).
  static void init_process();
  static auto run_subprocess_if_requested(int argc, char** argv) -> void;
  // Dispatch the private build-client mode before starting the evaluator.
  static auto build_client(int argc, char** argv) -> std::optional<int>;
  // `entry` is the trusted Card entry function. Each call supplies its scope.
  explicit NixEvaluator(std::string entry);
  ~NixEvaluator();

  // Evaluate only: writes the .drv and input paths, never builds an output.
  // Throws NixInterrupted for cancellation/deadline, otherwise runtime_error.
  auto evaluate(const NixCall& call) -> NixResult;
  // Validate observed evaluation inputs and hold both store paths before reuse.
  auto reuse(const NixCall& call, const NixResult& result) -> bool;
  // On the caller's thread; cancellation, deadlines and logs cover export too.
  auto make_available(const NixCall& call,
                      const DerivationClosureTransport& transport,
                      const WorkerDerivation& derivation) -> void;
  // Host-side native-worker preparation, through nix-daemon in the shared
  // store, on the caller's thread.
  auto realise_native(const NixCall& call, NixResult result) -> NixResult;

  auto expression(const NixCall& call, const std::string& expression,
                  bool build) -> ExpressionResult;
  auto lock_source(const std::string& url) -> NixSource;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Thrown by NixEvaluator for a cancelled or timed out call.
struct NixInterrupted : std::runtime_error {
  bool timed_out;
  NixInterrupted(bool t, const std::string& m)
      : std::runtime_error(m), timed_out(t) {}
};

struct NixImplementationLimit : std::runtime_error {
  using std::runtime_error::runtime_error;
};

inline constexpr size_t kMaxImplementationLabelBytes = 200;

// Labels name implementations in history and comparison: each is optional,
// but a given label names one implementation only.
inline auto check_implementation_labels(const std::vector<std::string>& labels,
                                        uint64_t count) -> void {
  if (labels.size() != count)
    throw std::runtime_error(
        "the Card plan has a different number of implementation labels");
  std::set<std::string_view> seen;
  for (const auto& label : labels) {
    if (label.empty()) continue;
    if (label.size() > kMaxImplementationLabelBytes)
      throw std::runtime_error("run list label \"" + label.substr(0, 40) +
                               "...\" is longer than 200 bytes");
    if (std::ranges::any_of(
            label, [](unsigned char c) { return c < 0x20 || c == 0x7f; }))
      throw std::runtime_error("run list label \"" + label +
                               "\" contains a control character");
    if (!seen.insert(label).second)
      throw std::runtime_error("run list label \"" + label +
                               "\" names more than one implementation");
  }
}

inline auto enforce_implementation_limit(uint64_t count, uint32_t limit)
    -> void {
  if (count > limit)
    throw NixImplementationLimit(
        "input and run list expansion exceeds 256 combinations");
}

}  // namespace tsugumi
