#include <nix/store/store-api.hh>
#include <nix/store/store-open.hh>
#include <stdexcept>

#include "tsugumi/workerd/derivation.hh"

namespace tsugumi {

auto BinaryCacheDerivationTransport::send(
    const WorkerDerivation& derivation) const -> void {
  if (!publish_) throw std::logic_error("missing Nix cache publisher");
  auto store = nix::openStore();
  const auto drv = store->parseStorePath(derivation.path);
  if (!drv.isDerivation())
    throw std::runtime_error("remote Nix store path is not a derivation");
  store->addTempRoot(drv);
  publish_(store->config.getReference().render(), derivation.path);
}

auto BinaryCacheDerivationTransport::receive(
    nix::Store& store, const WorkerDerivation& derivation) const -> void {
  const auto drv = store.parseStorePath(derivation.path);
  if (!drv.isDerivation())
    throw std::runtime_error("remote Nix store path is not a derivation");
  store.addTempRoot(drv);
  store.ensurePath(drv);
}

}  // namespace tsugumi
