#include "tsugumi/workerd/cache_relay_store.hh"

#include <mutex>
#include <nix/store/binary-cache-store.hh>
#include <nix/store/store-open.hh>
#include <nix/util/serialise.hh>

namespace tsugumi {
namespace {

struct RelayConfig final : nix::BinaryCacheStoreConfig {
  explicit RelayConfig(CacheRelay::Exchange exchange)
      : nix::StoreConfig(nix::StoreReference::Params{}),
        nix::BinaryCacheStoreConfig(nix::StoreReference::Params{}),
        exchange(std::move(exchange)) {}

  auto openStore() const -> nix::ref<nix::Store> override;
  auto getReference() const -> nix::StoreReference override {
    return {.variant = nix::StoreReference::Specified{
                .scheme = "tsugumi-cache", .authority = "broker"},
            .params = {}};
  }
  auto getHumanReadableURI() const -> std::string override {
    return "tsugumi-cached";
  }

  const CacheRelay::Exchange exchange;
};

class RelayStore final : public nix::BinaryCacheStore {
 public:
  explicit RelayStore(nix::ref<RelayConfig> config)
      : nix::Store(*config),
        nix::BinaryCacheStore(*config),
        config_(std::move(config)) {}

  auto isTrustedClient() -> std::optional<nix::TrustedFlag> override {
    return std::nullopt;
  }

  auto fileExists(const std::string& path) -> bool override {
    std::lock_guard lock(exchange_mutex_);
    return config_->exchange(CacheRelay::Method::head, path, 0, {}, {});
  }

  auto getFile(const std::string& path, nix::Sink& sink) -> void override {
    std::lock_guard lock(exchange_mutex_);
    if (!config_->exchange(CacheRelay::Method::get, path, 0, {},
                           [&](auto bytes) { sink(bytes); }))
      throw nix::NoSuchBinaryCacheFile(
          "file '%s' is absent from tsugumi-cached", path);
  }

  auto upsertFile(const std::string& path, nix::RestartableSource& source,
                  const std::string&, uint64_t size_hint) -> void override {
    std::lock_guard lock(exchange_mutex_);
    if (!config_->exchange(CacheRelay::Method::put, path, size_hint,
                           [&](auto buffer) {
                             return source.read(buffer.data(), buffer.size());
                           },
                           {}))
      throw nix::Error("tsugumi-cached did not store '%s'", path);
  }

 private:
  // Nix uses several threads. The worker relay carries one request at a time.
  std::mutex exchange_mutex_;
  nix::ref<RelayConfig> config_;
};

auto RelayConfig::openStore() const -> nix::ref<nix::Store> {
  auto store = nix::make_ref<RelayStore>(nix::make_ref<RelayConfig>(exchange));
  store->init();
  return store;
}

}  // namespace

auto open_relay_cache(CacheRelay::Exchange exchange)
    -> std::shared_ptr<nix::Store> {
  return RelayConfig(std::move(exchange)).openStore().get_ptr();
}
auto publish_to_relay_cache(const std::string& source_store,
                            const std::string& store_path,
                            CacheRelay::Exchange exchange) -> void {
  auto source =
      source_store.empty() ? nix::openStore() : nix::openStore(source_store);
  auto target = open_relay_cache(std::move(exchange));
  const auto path = source->parseStorePath(store_path);
  source->addTempRoot(path);
  nix::copyClosure(*source, *target, nix::StorePathSet{path});
}

}  // namespace tsugumi
