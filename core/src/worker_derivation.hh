// The contract produced only by host-side tsugumid, for either worker kind.
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <utility>

namespace nix {
class Store;
}

namespace tsugumi {

struct NixResult;

enum class WorkerKind { native, remote };

struct ExecutionTarget {
  WorkerKind kind = WorkerKind::native;
  std::string system;
};

struct WorkerDerivation {
  std::string path;    // .drv path, with its input closure made available
  std::string output;  // expected Card manifest output
  std::string system;
};

// Store availability is independent of evaluation and invocation scheduling.
// A future binary-cache transport implements this interface instead of archives.
class DerivationClosureTransport {
 public:
  virtual ~DerivationClosureTransport() = default;
  virtual auto send(const WorkerDerivation& derivation) const -> void = 0;
  virtual auto receive(nix::Store& store, const WorkerDerivation& derivation) const -> void = 0;
};

// Native workers share the host's store paths; nothing crosses stores.
class SharedStoreDerivationTransport final : public DerivationClosureTransport {
 public:
  auto send(const WorkerDerivation&) const -> void override {}
  auto receive(nix::Store&, const WorkerDerivation&) const -> void override {}
};

// The remote worker's current transport: an exported input-closure archive.
class ArchiveDerivationTransport final : public DerivationClosureTransport {
 public:
  explicit ArchiveDerivationTransport(std::filesystem::path archive) : archive_(std::move(archive)) {}
  auto send(const WorkerDerivation& derivation) const -> void override;
  auto receive(nix::Store& store, const WorkerDerivation& derivation) const -> void override;

 private:
  std::filesystem::path archive_;
};

}  // namespace tsugumi
