// Store operations only. Card evaluation belongs exclusively to tsugumid.
#include "nix_store.hh"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>

#include <algorithm>
#include <cerrno>
#include <memory>
#include <mutex>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <nix/util/signals.hh>
#include <nix/store/derivations.hh>
#include <nix/store/derived-path.hh>
#include <nix/store/globals.hh>
#include <nix/store/store-api.hh>
#include <nix/store/store-open.hh>
#include <nix/util/file-descriptor.hh>
#include <nix/util/logging.hh>
#include <nix/util/terminal.hh>
#include <sstream>

#include "nix_eval.hh"
#include "model.hh"

namespace tsugumi {
namespace {
class WorkerLogger final : public nix::Logger {
 public:
  explicit WorkerLogger(std::function<void(const std::string&)> sink)
      : sink_(std::move(sink)) {}
  auto clear() -> void {
    std::lock_guard lock(mutex_);
    sink_ = {};
  }
  auto log(nix::Verbosity level, std::string_view text) -> void override {
    if (level <= nix::lvlInfo) emit(std::string(text));
  }
  auto logEI(const nix::ErrorInfo& info) -> void override {
    std::ostringstream text;
    nix::showErrorInfo(text, info, false);
    emit(text.str());
  }
  auto startActivity(nix::ActivityId, nix::Verbosity level, nix::ActivityType,
                     const std::string& text, const Fields&, nix::ActivityId)
      -> void override {
    if (level <= nix::lvlInfo && !text.empty()) emit(text);
  }
  auto result(nix::ActivityId, nix::ResultType type, const Fields& fields)
      -> void override {
    if ((type == nix::resBuildLogLine || type == nix::resPostBuildLogLine) &&
        !fields.empty() && fields[0].type == Field::tString)
      emit(fields[0].s);
  }

 private:
  auto emit(const std::string& text) -> void {
    std::lock_guard lock(mutex_);
    if (sink_) sink_(nix::filterANSIEscapes(text, true) + "\n");
  }
  std::mutex mutex_;
  std::function<void(const std::string&)> sink_;
};
}  // namespace

auto RemoteWorkerStore::realise(
    const DerivationClosureTransport& transport,
    const std::filesystem::path& store_root, const WorkerDerivation& derivation,
    const std::function<void(const std::string&)>& log, uint64_t timeout_ms) -> NixResult {
  // Invocations run in separate processes but share this persistent store.
  // Serialize its entire connection lifetime, including database creation and
  // closure import. The lock ends before Card execution, so Cards can still run
  // concurrently. Never unlink it: surviving supervisors also use this inode.
  nix::AutoCloseFD store_lock{::open((store_root / "realise.lock").c_str(),
                                    O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
                                    0600)};
  if (!store_lock)
    throw std::runtime_error("cannot open remote Nix store lock");
  while (::flock(store_lock.get(), LOCK_EX) != 0) {
    if (errno == EINTR) continue;
    throw std::runtime_error("cannot lock remote Nix store");
  }
  nix::initLibStore(true);
  auto logger = std::make_unique<WorkerLogger>(log);
  auto* sink = logger.get();
  nix::logger = std::move(logger);
  struct ClearLog {
    WorkerLogger* sink;
    ~ClearLog() { sink->clear(); }
  } clear_log{sink};
  // The private store survives invocation/supervisor death. Persist imported
  // and built paths before Nix registers them, just as in the shared VM.
  nix::settings.getLocalSettings().fsyncStorePaths = true;
  nix::settings.getLocalSettings().fsyncMetadata = true;
  nix::settings.getLocalSettings().sandboxMode = nix::smEnabled;
  nix::settings.getLocalSettings().sandboxFallback = false;
  // This process is the only user of its namespace. For a root supervisor Nix
  // would otherwise give the store to the host's build-users-group, which is
  // not mapped here, and require setgroups, which the namespace denies.
  nix::settings.getLocalSettings().buildUsersGroup = "";
  nix::settings.getLocalSettings().requireDropSupplementaryGroups = false;
  auto store = nix::openStore("local", {{"root", store_root.string()}});
  const auto drv = store->parseStorePath(derivation.path);
  const auto out = store->parseStorePath(derivation.output);
  store->addTempRoot(drv);
  store->addTempRoot(out);
  transport.receive(*store, derivation);
  const auto began = std::chrono::steady_clock::now();
  const auto thread = pthread_self();
  struct sigaction action{};
  action.sa_handler = +[](int) {};
  sigemptyset(&action.sa_mask);
  ::sigaction(SIGUSR1, &action, nullptr);
  std::atomic<bool> timed_out{false};
  nix::unix::interruptCheck = [&] { return timed_out.load(); };
  struct ClearInterrupt { ~ClearInterrupt() { nix::unix::interruptCheck = {}; } } clear_interrupt;
  std::mutex timer_mutex;
  std::condition_variable timer_cv;
  std::jthread timer([&](std::stop_token stop) {
    std::unique_lock lock(timer_mutex);
    while (!stop.stop_requested()) {
      if (timeout_ms && std::chrono::steady_clock::now() - began >= std::chrono::milliseconds(timeout_ms)) {
        timed_out.store(true);
        ::pthread_kill(thread, SIGUSR1);
      }
      timer_cv.wait_for(lock, std::chrono::milliseconds(50));
    }
  });
  struct StopTimer {
    std::jthread& timer;
    std::condition_variable& cv;
    ~StopTimer() { timer.request_stop(); cv.notify_all(); timer.join(); }
  } stop_timer{timer, timer_cv};
  if (store->readDerivation(drv).platform != derivation.system)
    throw std::runtime_error("remote derivation has a different system");
  const auto declared = store->queryDerivationOutputMap(drv);
  const auto declared_out = declared.find("out");
  if (declared_out == declared.end() || declared_out->second != out)
    throw std::runtime_error(
        "remote Nix derivation has a different output path");
  NixResult result;
  if (!store->isValidPath(out)) {
    try {
      store->buildPaths(
          {nix::DerivedPath::Built{.drvPath = nix::makeConstantStorePathRef(drv),
                                   .outputs = nix::OutputsSpec::All{}}});
    } catch (...) {
      if (timed_out.load()) throw Failed(FailureKind::timeout, "Nix exceeded buildTimeoutSeconds");
      throw;
    }
    result.built = true;
  }
  if (!store->isValidPath(out))
    throw std::runtime_error("remote Nix build produced no Card manifest");
  nix::StorePathSet closure;
  store->computeFSClosure(out, closure);
  for (const auto& path : closure)
    result.closure.push_back(store->printStorePath(path));
  std::sort(result.closure.begin(), result.closure.end());
  const auto manifest =
      store_root / std::filesystem::path(derivation.output).relative_path();
  const int fd =
      ::open(manifest.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0)
    throw std::runtime_error("cannot open the remote Nix Card manifest");
  nix::AutoCloseFD file{fd};
  struct stat info{};
  if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
      info.st_size > 16 * 1024 * 1024)
    throw std::runtime_error("invalid remote Nix Card manifest size or type");
  result.manifest.resize(static_cast<size_t>(info.st_size));
  size_t offset = 0;
  while (offset < result.manifest.size()) {
    const ssize_t n = ::read(fd, result.manifest.data() + offset,
                             result.manifest.size() - offset);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0)
      throw std::runtime_error("cannot read the remote Nix Card manifest");
    offset += static_cast<size_t>(n);
  }
  result.derivation = derivation;
  return result;
}

}  // namespace tsugumi
