// The contract produced only by host-side tsugumid, for either worker kind.
#pragma once

#include <functional>
#include <string>
#include <utility>

namespace nix {
class Store;
}

namespace tsugumi {

enum class WorkerKind { native, remote };

struct ExecutionTarget {
  WorkerKind kind = WorkerKind::native;
  std::string system;
};

struct WorkerDerivation {
  std::string path;    // .drv path, with its input closure made available
  std::string output;  // expected Card build plan output
  std::string system;
};

// Store availability is independent of evaluation and invocation scheduling.
class DerivationClosureTransport {
 public:
  virtual ~DerivationClosureTransport() = default;
  virtual auto send(const WorkerDerivation& derivation) const -> void = 0;
  virtual auto receive(nix::Store& store,
                       const WorkerDerivation& derivation) const -> void = 0;
};

// Native workers share the host's store paths; nothing crosses stores.
class SharedStoreDerivationTransport final : public DerivationClosureTransport {
 public:
  auto send(const WorkerDerivation&) const -> void override {}
  auto receive(nix::Store&, const WorkerDerivation&) const -> void override {}
};

// The publisher copies a store closure to the host-selected cache. The
// receiving store is configured with the job's substituters and trusted keys.
class BinaryCacheDerivationTransport final : public DerivationClosureTransport {
 public:
  using Publish = std::function<void(const std::string&, const std::string&)>;
  explicit BinaryCacheDerivationTransport(Publish publish = {})
      : publish_(std::move(publish)) {}
  auto send(const WorkerDerivation& derivation) const -> void override;
  auto receive(nix::Store& store, const WorkerDerivation& derivation) const
      -> void override;

 private:
  Publish publish_;
};

}  // namespace tsugumi
