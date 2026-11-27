#include "tsugumi/workerd/nix_log.hh"
// Store operations only. Card evaluation belongs exclusively to tsugumid.
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <nix/store/build-result.hh>
#include <nix/store/derivations.hh>
#include <nix/store/derived-path.hh>
#include <nix/store/filetransfer.hh>
#include <nix/store/globals.hh>
#include <nix/store/store-api.hh>
#include <nix/store/store-open.hh>
#include <nix/util/config-global.hh>
#include <nix/util/file-descriptor.hh>
#include <nix/util/logging.hh>
#include <nix/util/signals.hh>
#include <nix/util/terminal.hh>
#include <sstream>
#include <thread>

#include "nix_store.hh"
#include "tsugumi/workerd/cache_publish.hh"
#include "tsugumi/workerd/store_result.hh"
#include "tsugumi/workerd/values.hh"

namespace tsugumi {
namespace {
auto ValidCacheField(std::string_view field) -> bool {
  return !field.empty() && std::ranges::all_of(field, [](unsigned char c) {
    return c > 32 && c < 127 && c != '"' && c != '\\' && c != '#';
  });
}

auto ConfigurePulls(const workerd::R::CacheSettingsT& cache)
    -> nix::AutoCloseFD {
  // Keep the job's trust policy independent of the worker machine's Nix
  // configuration, including store URLs that opt out of signature checks.
  std::string substituters = "https://cache.nixos.org/";
  std::string public_keys =
      "cache.nixos.org-1:6NCHdD59X431o0gWypbMrAURkbJ16ZPMQFGspcDShjY=";
  std::string netrc;
  for (const auto& entry : cache.pull) {
    const auto& endpoint = entry->endpoint;
    if (!ValidCacheField(endpoint) ||
        endpoint.find_first_of("?@") != endpoint.npos ||
        !(endpoint.starts_with("https://") || endpoint.starts_with("http://") ||
          endpoint.starts_with("file:///")) ||
        !ValidCacheField(entry->public_key))
      throw std::runtime_error("invalid worker cache pull endpoint or key");
    substituters += ' ' + endpoint;
    public_keys += ' ' + entry->public_key;
    if (!entry->secret.empty()) {
      if (!endpoint.starts_with("https://") || !ValidCacheField(entry->host) ||
          !ValidCacheField(entry->user) || !ValidCacheField(entry->secret))
        throw std::runtime_error("invalid worker cache pull credentials");
      netrc += "machine " + entry->host + " login " + entry->user +
               " password " + entry->secret + '\n';
    }
  }
  nix::globalConfig.set("substituters", substituters);
  if (!cache.own_signing_key.empty()) {
    if (!ValidCacheField(cache.own_signing_key))
      throw std::runtime_error("invalid built-in binary cache signing key");
    public_keys += ' ' + cache.own_signing_key;
  }
  nix::globalConfig.set("trusted-public-keys", public_keys);
  nix::settings.requireSigs = true;
  if (netrc.empty()) {
    nix::fileTransferSettings.netrcFile = "/dev/null";
    return {};
  }
  nix::AutoCloseFD file{::memfd_create("tsugumi-cache-netrc", MFD_CLOEXEC)};
  if (!file)
    throw std::runtime_error("cannot create the worker's private netrc");
  nix::writeFull(file.get(), netrc);
  workerd::clear_secret(netrc);
  nix::fileTransferSettings.netrcFile =
      "/proc/self/fd/" + std::to_string(file.get());
  return file;
}
}  // namespace

auto RemoteWorkerStore::realise(
    const DerivationClosureTransport& transport,
    const workerd::R::CacheSettingsT& caches, CacheRelay::Exchange relay,
    const std::filesystem::path& store_root, const WorkerDerivation& derivation,
    bool push_outputs, const std::function<void(const std::string&)>& log,
    uint64_t timeout_ms, const StoreProgress& progress) -> WorkerStoreResult {
  // Invocations run in separate supervisor processes that share this
  // persistent store, so the Nix settings below are per process. Opening the
  // store is serialized: LocalStore creates a missing database and its schema,
  // and runs migrations, under only a shared big-lock, so two first-time
  // openers could race. Everything after the open is safe without this lock:
  // SQLite and its retries guard the database, temp roots are per process,
  // and every addition (closure import, substitution, build) takes the
  // output's path lock and checks validity again under it, so two invocations
  // wanting one path produce it once. Nothing collects this store's garbage
  // (automatic collection is turned off below), and job removal touches only
  // job scratch. Never unlink the lock: surviving
  // supervisors also use this inode.
  nix::AutoCloseFD store_lock{::open((store_root / "realise.lock").c_str(),
                                     O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
                                     0600)};
  if (!store_lock)
    throw std::runtime_error("cannot open remote Nix store lock");
  if (progress.waiting && ::flock(store_lock.get(), LOCK_EX | LOCK_NB) != 0)
    progress.waiting();
  while (::flock(store_lock.get(), LOCK_EX) != 0) {
    if (errno == EINTR) continue;
    throw std::runtime_error("cannot lock remote Nix store");
  }
  if (progress.locked) progress.locked();
  nix::initLibStore(true);
  const auto netrc = ConfigurePulls(caches);
  std::mutex provenance;
  std::string substituted_from;
  auto logger = std::make_unique<NixLogSink>(log);
  logger->set_substitutions([&](const auto& path, const auto& uri) {
    if (progress.substitute) progress.substitute(path);
    if (path != derivation.output) return;
    std::lock_guard lock(provenance);
    substituted_from = text::public_cache_endpoint(uri);
    if (uri.starts_with("file://")) substituted_from = "file://";
  });
  if (progress.build) logger->set_thread_builds(&progress.build);
  auto* sink = logger.get();
  nix::logger = std::move(logger);
  struct ClearLog {
    NixLogSink* sink;
    ~ClearLog() {
      sink->set({});
      sink->set_substitutions({});
      sink->set_thread_builds(nullptr);
    }
  } clear_log{sink};
  nix::settings.getLocalSettings().sandboxMode = nix::smEnabled;
  nix::settings.getLocalSettings().sandboxFallback = false;
  // This process is the only user of its namespace. For a root supervisor Nix
  // would otherwise give the store to the host's build-users-group, which is
  // not mapped here, and require setgroups, which the namespace denies.
  nix::settings.getLocalSettings().buildUsersGroup = "";
  nix::settings.getLocalSettings().requireDropSupplementaryGroups = false;
  // Never collect garbage automatically, whatever min-free the host's
  // nix.conf sets: once realise returns the store closes and its temp roots
  // go away while the Card still uses these paths, and other supervisors use
  // paths of their own that no root protects. With min-free 0, max-free is
  // never consulted.
  nix::settings.getLocalSettings().getGCSettings().minFree = 0;
  auto store = nix::openStore(
      "local", {{"root", store_root.string()}, {"require-sigs", "true"}});
  store_lock.close();
  const auto drv = store->parseStorePath(derivation.path);
  const auto out = store->parseStorePath(derivation.output);
  store->addTempRoot(drv);
  store->addTempRoot(out);
  auto own = relay ? open_relay_cache(std::move(relay)) : nullptr;
  const auto substitute = [&](const auto& path) {
    if (!own || store->isValidPath(path) || !own->isValidPath(path))
      return false;
    nix::copyClosure(*own, *store, nix::StorePathSet{path}, nix::NoRepair,
                     nix::CheckSigs);
    return true;
  };
  substitute(drv);
  transport.receive(*store, derivation);
  const auto began = std::chrono::steady_clock::now();
  const auto thread = pthread_self();
  struct sigaction action{};
  action.sa_handler = +[](int) {};
  sigemptyset(&action.sa_mask);
  ::sigaction(SIGUSR1, &action, nullptr);
  std::atomic<bool> timed_out{false};
  nix::unix::interruptCheck = [&] { return timed_out.load(); };
  struct ClearInterrupt {
    ~ClearInterrupt() { nix::unix::interruptCheck = {}; }
  } clear_interrupt;
  std::mutex timer_mutex;
  std::condition_variable_any timer_cv;
  std::jthread timer;
  if (timeout_ms)
    timer = std::jthread([&](std::stop_token stop) {
      std::unique_lock lock(timer_mutex);
      const auto no_condition = [] { return false; };
      timer_cv.wait_until(lock, stop,
                          began + std::chrono::milliseconds(timeout_ms),
                          no_condition);
      while (!stop.stop_requested()) {
        timed_out.store(true);
        ::pthread_kill(thread, SIGUSR1);
        timer_cv.wait_for(lock, stop, std::chrono::milliseconds(50),
                          no_condition);
      }
    });
  if (store->readDerivation(drv).platform != derivation.system)
    throw std::runtime_error("remote derivation has a different system");
  const auto declared = store->queryDerivationOutputMap(drv);
  const auto declared_out = declared.find("out");
  if (declared_out == declared.end() || declared_out->second != out)
    throw std::runtime_error(
        "remote Nix derivation has a different output path");
  WorkerStoreResult result;
  if (substitute(out)) result.substituter = "tsugumi-cached";
  if (!store->isValidPath(out)) {
    try {
      auto builds = store->buildPathsWithResults({nix::DerivedPath::Built{
          .drvPath = nix::makeConstantStorePathRef(drv),
          .outputs = nix::OutputsSpec::All{}}});
      if (builds.size() != 1)
        throw std::runtime_error("missing remote Nix build result");
      builds.front().tryThrowBuildError();
      const auto status = builds.front().tryGetSuccess()->status;
      result.built = status == nix::BuildResult::Success::Built;
      if (status == nix::BuildResult::Success::Substituted) {
        std::lock_guard lock(provenance);
        result.substituter = substituted_from;
      }
    } catch (...) {
      if (timed_out.load())
        throw Failed(FailureKind::timeout, "Nix exceeded buildTimeoutSeconds");
      throw;
    }
  }
  if (!store->isValidPath(out))
    throw std::runtime_error("remote Nix build produced no Card build plan");
  if (timer.joinable()) {
    timer.request_stop();
    timer.join();
  }
  if (push_outputs) {
    try {
      if (own) {
        nix::copyClosure(*store, *own, nix::StorePathSet{out});
      } else {
        const auto& push = *caches.push;
        using Kind = workerd::R::CacheTargetKind;
        const auto kind = push.kind == Kind::Attic ? ExternalCacheKind::attic
                          : push.kind == Kind::Cachix
                              ? ExternalCacheKind::cachix
                              : ExternalCacheKind::nix_copy;
        publish_to_external_cache(
            {kind, push.endpoint, push.cache_name, push.user, push.secret},
            std::filesystem::temp_directory_path(),
            {.store = store->config.getReference().render(),
             .private_worker_store = true},
            derivation.output, {});
      }
      result.pushed = true;
    } catch (const std::exception&) {
      log("Could not publish the Nix build outputs to the configured cache.\n");
    }
  }
  nix::StorePathSet closure;
  store->computeFSClosure(out, closure);
  for (const auto& path : closure)
    result.closure.push_back(store->printStorePath(path));
  std::sort(result.closure.begin(), result.closure.end());
  const auto plan =
      store_root / std::filesystem::path(derivation.output).relative_path();
  const int fd =
      ::open(plan.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0)
    throw std::runtime_error("cannot open the remote Nix Card build plan");
  nix::AutoCloseFD file{fd};
  struct stat info{};
  if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
      info.st_size > 16 * 1024 * 1024)
    throw std::runtime_error("invalid remote Nix Card build plan size or type");
  result.plan.resize(static_cast<size_t>(info.st_size));
  size_t offset = 0;
  while (offset < result.plan.size()) {
    const ssize_t n =
        ::read(fd, result.plan.data() + offset, result.plan.size() - offset);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0)
      throw std::runtime_error("cannot read the remote Nix Card build plan");
    offset += static_cast<size_t>(n);
  }
  result.derivation = derivation;
  return result;
}

}  // namespace tsugumi
