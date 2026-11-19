// The Nix evaluator inside tsugumid (libnixexpr / libnixstore): one evaluator
// and store connection per process, used from a single thread of its own, so
// that nixpkgs is read once and evaluation state is reused between Cards and
// runs. Transfers use libnix on the caller's thread. Native builds use a
// separate Nix client so each connection has its own build options.
#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "tsugumi/workerd/store_result.hh"

namespace tsugumi {

inline constexpr uint32_t kMaxExecutionBatchSize = 256;

// Settings and entry source belong to one evaluator epoch; observed reads are
// checked by reuse(). Operational limits/deadlines are applied on every call.
using EvaluationKey =
    std::tuple<std::string, std::string, std::string, std::string>;

using NixOutput = std::function<void(std::span<const std::string>)>;

struct NixCall {
  std::string pin_url;   // nixpkgs: a fetchTarball URL
  std::string pin_hash;  // nixpkgs: its sha256 (SRI)
  std::string args;  // Nix expression of the entry's arguments (sdk, gen, card)
  // Confirmed definition sources, also available to alternative build backends
  // without parsing the Nix expression above.
  std::filesystem::path definition_dir;
  std::string source_overrides = "{}";  // approved JSON package customizations
  std::string system;  // explicit Card target system; flake lookup may use the
                       // host default
  // GC root created for the result: <root_dir>/<root_prefix><result name>, a
  // symlink (registered as an indirect root outside /nix/var/nix/gcroots).
  std::filesystem::path root_dir;
  std::string root_prefix;
  std::optional<uint32_t> jobs;
  std::optional<uint32_t> cores;
  uint32_t max_implementations = kMaxExecutionBatchSize;
  const std::atomic<bool>* cancel = nullptr;
  std::optional<std::chrono::steady_clock::time_point> deadline;
  NixOutput log;  // evaluation messages and build output
  std::function<void(bool building)>
      on_phase;  // false: evaluating, true: building
  auto evaluation_key() const -> EvaluationKey {
    return {pin_url, pin_hash, system, args};
  }
};

struct NixResult : WorkerStoreResult {
  bool execution_list = false;
  uint32_t implementation_count = 1;
  uint64_t evaluation_epoch = 0;
};

struct FlakeInfo {
  std::string ref;  // locked: a commit and its narHash
  std::string rev, nar_hash;
  std::vector<std::string> packages;  // packages.<this system>, by attribute
  // {"ref", "rev", "narHash", "packages": {<attr>: {"name", "drv", "out",
  // "output"}}}; the .drv files are in the store.
  std::string json;
};

class NixEvaluator {
 public:
  // Must run on the main thread before other threads start (Boehm GC).
  static void init_process();
  // `entry` is the entry function's source; `allowed` the folders evaluation
  // may read (restricted mode): the SDK and the evaluation folder.
  NixEvaluator(std::string entry, std::vector<std::string> allowed);
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

  // A flake a project opted in to, evaluated apart from every Card: its own
  // state has getFlake and runs as `nix build` does (pure, no IFD). A Card only
  // ever gets the result. `ref` may be unlocked; the result's is locked.
  // `call.source_overrides` contains approved JSON override/overrideAttrs maps,
  // applied here before derivations cross into the restricted Card evaluator.
  // It also supplies the target system, cancellation, deadline and log.
  FlakeInfo flake(const NixCall& call, const std::string& ref);

  // Resident memory of the evaluator's heap (Boehm GC), in bytes.

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

inline auto enforce_implementation_limit(uint64_t count, uint32_t limit)
    -> void {
  if (count > limit)
    throw NixImplementationLimit(
        "input and run list expansion exceeds 256 combinations");
}

}  // namespace tsugumi
