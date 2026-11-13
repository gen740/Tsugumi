#include "worker_derivation.hh"

#include <fcntl.h>

#include <stdexcept>

#include <nix/store/export-import.hh>
#include <nix/store/store-api.hh>
#include <nix/store/store-open.hh>
#include <nix/util/file-descriptor.hh>
#include <nix/util/serialise.hh>

namespace tsugumi {

auto ArchiveDerivationTransport::send(const WorkerDerivation& derivation) const -> void {
  auto store = nix::openStore();
  const auto drv = store->parseStorePath(derivation.path);
  store->addTempRoot(drv);
  nix::StorePathSet inputs;
  store->computeFSClosure(drv, inputs);
  const int fd = ::open(archive_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) throw std::runtime_error("cannot create remote Nix derivation archive");
  nix::AutoCloseFD file{fd};
  nix::FdSink sink{fd};
  nix::exportPaths(*store, inputs, sink);
  sink.flush();
}

auto ArchiveDerivationTransport::receive(nix::Store& store, const WorkerDerivation& derivation) const -> void {
  if (store.isValidPath(store.parseStorePath(derivation.path))) return;
  const int fd = ::open(archive_.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) throw std::runtime_error("cannot read the remote Nix derivation archive");
  nix::AutoCloseFD file{fd};
  nix::FdSource source{fd};
  // The daemon's private store accepts paths bound to this job by SHA-256.
  nix::importPaths(store, source, nix::NoCheckSigs);
}

}  // namespace tsugumi
