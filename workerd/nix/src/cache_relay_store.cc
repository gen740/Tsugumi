#include "tsugumi/workerd/cache_relay_store.hh"

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include <filesystem>
#include <mutex>
#include <nix/store/binary-cache-store.hh>
#include <nix/store/nar-info.hh>
#include <nix/store/store-open.hh>
#include <nix/util/compression.hh>
#include <nix/util/file-descriptor.hh>
#include <nix/util/hash.hh>
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

  using nix::BinaryCacheStore::upsertFile;

  // Writes one path as a Nix binary cache store does, except that its
  // references need not be in this cache.
  auto upload(nix::Store& source, const nix::StorePath& path) -> void {
    const auto info = source.queryPathInfo(path);
    auto pattern =
        (std::filesystem::temp_directory_path() / "tsugumi-nar.XXXXXX")
            .string();
    nix::AutoCloseFD file{::mkstemp(pattern.data())};
    if (!file) throw nix::Error("cannot stage a NAR for tsugumi-cached");
    ::unlink(pattern.c_str());
    nix::HashSink file_hash(nix::HashAlgorithm::SHA256);
    {
      nix::FdSink staged(file.get());
      nix::TeeSink both(staged, file_hash);
      auto compress = nix::makeCompressionSink(nix::CompressionAlgo::zstd, both);
      source.narFromPath(path, *compress);
      compress->finish();
      staged.flush();
    }
    const auto [hash, size] = file_hash.finish();
    if (::lseek(file.get(), 0, SEEK_SET) != 0)
      throw nix::Error("cannot read a staged NAR for tsugumi-cached");
    auto narinfo = nix::NarInfo(*info);
    narinfo.compression = "zstd";
    narinfo.fileHash = hash;
    narinfo.fileSize = size;
    narinfo.url =
        "nar/" + hash.to_string(nix::HashFormat::Nix32, false) + ".nar.zst";
    narinfo.sigs.clear();
    if (!fileExists(narinfo.url)) {
      nix::FdSource body(file.get());
      upsertFile(narinfo.url, body, "application/x-nix-nar", size);
    }
    upsertFile(std::string(path.hashPart()) + ".narinfo",
               narinfo.to_string(*this), "text/x-nix-narinfo");
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
                            CacheRelay::Exchange exchange,
                            const CacheTransferReport& report)
    -> CachePushPlan {
  auto source =
      source_store.empty() ? nix::openStore() : nix::openStore(source_store);
  auto target = open_relay_cache(std::move(exchange));
  source->addTempRoot(source->parseStorePath(store_path));
  auto plan = plan_cache_push(*source, store_path, target.get());
  copy_to_relay_cache(*source, *target, plan, report);
  return plan;
}
auto copy_to_relay_cache(nix::Store& source, nix::Store& relay,
                         const CachePushPlan& plan,
                         const CacheTransferReport& report) -> void {
  auto& target = dynamic_cast<RelayStore&>(relay);
  CacheTransferProgress progress{0, plan.paths.size(), 0, plan.bytes, {}};
  for (const auto& printed : plan.paths) {
    const auto path = source.parseStorePath(printed);
    progress.path = printed;
    if (report) report(progress);
    target.upload(source, path);
    ++progress.done;
    progress.bytes_done += source.queryPathInfo(path)->narSize;
  }
  progress.path.clear();
  if (report) report(progress);
}
auto plan_store_push(const std::string& source_store,
                     const std::string& store_path) -> CachePushPlan {
  auto source =
      source_store.empty() ? nix::openStore() : nix::openStore(source_store);
  source->addTempRoot(source->parseStorePath(store_path));
  return plan_cache_push(*source, store_path, nullptr);
}

}  // namespace tsugumi
