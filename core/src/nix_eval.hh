// The Nix evaluator inside tsugumid (libnixexpr / libnixstore): one evaluator
// and store connection per process, used from a single thread of its own, so
// that nixpkgs is read once and evaluation state is reused between Cards and
// runs. Builds go to the Nix daemon. No Nix CLI is involved.
#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace tsugumi {

struct NixCall {
  std::string pin_url;   // nixpkgs: a fetchTarball URL
  std::string pin_hash;  // nixpkgs: its sha256 (SRI)
  std::string args;      // Nix expression of the entry's arguments (sdk, gen, card)
  std::string system;    // the Nix system to evaluate for; "" = this machine's
  // GC root created for the result: <root_dir>/<root_prefix><result name>, a
  // symlink (registered as an indirect root outside /nix/var/nix/gcroots).
  std::filesystem::path root_dir;
  std::string root_prefix;
  const std::atomic<bool>* cancel = nullptr;
  std::optional<std::chrono::steady_clock::time_point> deadline;
  std::function<void(const std::string&)> log;  // evaluation messages and build output
  std::function<void(bool building)> on_phase;  // false: evaluating, true: building
};

struct NixResult {
  std::string out;       // store path of the Card manifest
  std::string manifest;  // its contents
  bool built = false;    // false: the result was already in the store
  std::filesystem::path root;
};

struct FlakeInfo {
  std::string ref;                    // locked: a commit and its narHash
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

  // Evaluates the Card, builds it unless the store has it, roots it.
  // Throws std::runtime_error with Nix's message; Interrupted as `cancelled`
  // or `timed_out` set in `what`.
  NixResult build(const NixCall& call);

  // A flake a project opted in to, evaluated apart from every Card: its own
  // state has getFlake and runs as `nix build` does (pure, no IFD). A Card only
  // ever gets the result. `ref` may be unlocked; the result's is locked. `call`
  // gives the cancel, deadline and log only.
  FlakeInfo flake(const NixCall& call, const std::string& ref);
  bool valid(const std::string& store_path);

  // Resident memory of the evaluator's heap (Boehm GC), in bytes.
  uint64_t heap_bytes();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Thrown by NixEvaluator for a cancelled or timed out call.
struct NixInterrupted : std::runtime_error {
  bool timed_out;
  NixInterrupted(bool t, const std::string& m) : std::runtime_error(m), timed_out(t) {}
};

}  // namespace tsugumi
