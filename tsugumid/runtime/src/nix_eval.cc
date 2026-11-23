#include "tsugumi/workerd/nix_log.hh"
// Built as C++23 (the Nix headers require it); the rest of the core is C++20
// and never includes a Nix header.
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <fstream>
#include <list>
#include <mutex>
#include <nix/expr/eval-gc.hh>
#include <nix/expr/eval-settings.hh>
#include <nix/expr/eval.hh>
#include <nix/expr/get-drvs.hh>
#include <nix/expr/json-to-value.hh>
#include <nix/expr/value-to-json.hh>
#include <nix/fetchers/fetch-settings.hh>
#include <nix/fetchers/fetchers.hh>
#include <nix/fetchers/filtering-source-accessor.hh>
#include <nix/store/derivations.hh>
#include <nix/store/derived-path.hh>
#include <nix/store/globals.hh>
#include <nix/store/indirect-root-store.hh>
#include <nix/store/remote-store.hh>
#include <nix/store/store-api.hh>
#include <nix/store/store-open.hh>
#include <nix/util/current-process.hh>
#include <nix/util/environment-variables.hh>
#include <nix/util/file-descriptor.hh>
#include <nix/util/finally.hh>
#include <nix/util/logging.hh>
#include <nix/util/serialise.hh>
#include <nix/util/signals.hh>
#include <nix/util/terminal.hh>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <thread>

#include "nix_eval.hh"
#include "nix_params.hh"
#include "tsugumi/protocol/nix_evaluator_generated.h"
#include "tsugumi/util/fs.hh"
#include "tsugumi/util/process.hh"
#include "tsugumi/workerd/log_limits.hh"
#include "tsugumi/workerd/store.hh"

extern char** environ;

namespace tsugumi {

namespace {

namespace E = Tsugumi::NixEvaluator;

constexpr size_t kExpressionBytes = 8u << 20;
constexpr size_t kExpressionEntries = 2000;
constexpr size_t kBuildLogTail = 256u << 10;

auto write_build_log(std::string_view line, bool truncated) -> void {
  flatbuffers::FlatBufferBuilder builder;
  const auto text = truncated ? flatbuffers::Offset<flatbuffers::String>{}
                              : builder.CreateString(line);
  builder.FinishSizePrefixed(E::CreateBuildLog(builder, text, truncated));
  nix::writeFull(STDOUT_FILENO,
                 {reinterpret_cast<const char*>(builder.GetBufferPointer()),
                  builder.GetSize()});
}

// Observe below Nix's allow-list, including negative lookups and directory
// enumeration. Keep observations for the entire EvalState: cached imports and
// lazy nixpkgs values can otherwise hide reads on subsequent evaluations.
class EvaluationReads : public nix::SourceAccessor {
 public:
  explicit EvaluationReads(nix::ref<nix::SourceAccessor> next)
      : next_(std::move(next)) {}
  bool reusable = true;
  // Collected in filesystem callbacks, rooted only after evaluation returns:
  // a callback may be serializing a source on an already-locked store
  // connection.
  std::set<std::string> store_roots;
  std::function<bool(const nix::CanonPath&)> allowed_path;

  auto unchanged() -> bool {
    if (!reusable) return false;
    for (auto& [key, value] : reads_) {
      const auto& [op, path] = key;
      if (observe(op, nix::CanonPath(path)) != value) return false;
    }
    return true;
  }

  auto readFile(const nix::CanonPath& p, nix::Sink& sink,
                nix::fun<void(uint64_t)> size) -> void override {
    auto bytes = next_->readFile(p);
    remember('f', p, digest(bytes));
    size(bytes.size());
    sink(bytes);
  }
  auto maybeLstat(const nix::CanonPath& p) -> std::optional<Stat> override {
    auto result = next_->maybeLstat(p);
    remember('s', p, stat_text(result));
    return result;
  }
  auto readDirectory(const nix::CanonPath& p) -> DirEntries override {
    auto result = next_->readDirectory(p);
    const bool filtered = std::any_of(
        result.begin(), result.end(),
        [&](const auto& item) { return !allowed_path(p / item.first); });
    // A store tree is immutable, but the allow-list's view of an ancestor
    // can grow when another evaluation grants an additional store path.
    remember('d', p, directory_digest(p, result), filtered);
    return result;
  }
  auto readLink(const nix::CanonPath& p) -> std::string override {
    auto result = next_->readLink(p);
    remember('l', p, result);
    return result;
  }
  auto getPhysicalPath(const nix::CanonPath& p)
      -> std::optional<std::filesystem::path> override {
    // A consumer could read this path without going through the accessor.
    // Store contents are immutable; other physical reads cannot be tracked.
    if (!store_path(p))
      reusable = false;
    else
      remember('s', p, stat_text(next_->maybeLstat(p)));
    return next_->getPhysicalPath(p);
  }

 private:
  using Key = std::pair<char, std::string>;
  nix::ref<nix::SourceAccessor> next_;
  std::map<Key, std::string> reads_;
  // Metadata is only a fast gate for rehashing: ctime also detects writes that
  // preserve mtime. Directories are always enumerated, not gated by mtime.
  std::map<std::string, std::pair<std::string, std::string>> hashes_;

  static auto digest(std::string_view bytes) -> std::string {
    return nix::hashString(nix::HashAlgorithm::SHA256, bytes)
        .to_string(nix::HashFormat::Base16, false);
  }
  static auto store_path(const nix::CanonPath& p) -> bool {
    // Only descendants of a store object are immutable, never /nix/store's
    // directory listing or a negative lookup of a not-yet-existing object.
    return p.abs().starts_with("/nix/store/");
  }
  static auto stat_text(const std::optional<Stat>& s) -> std::string {
    if (!s) return "missing";
    return std::to_string(s->type) + ":" +
           std::to_string(s->fileSize.value_or(0)) + ":" +
           std::to_string(s->isExecutable);
  }
  static auto directory_text(const DirEntries& entries) -> std::string {
    std::string result;
    for (const auto& [name, type] : entries)
      result += std::to_string(name.size()) + ":" + name + ":" +
                std::to_string(type ? static_cast<int>(*type) : -1) + ";";
    return digest(result);
  }
  auto directory_digest(const nix::CanonPath& p, DirEntries entries)
      -> std::string {
    std::erase_if(entries, [&](const auto& item) {
      return !allowed_path(p / item.first);
    });
    return directory_text(entries);
  }
  auto file_digest(const nix::CanonPath& p) -> std::string {
    struct stat st{};
    if (::lstat(p.c_str(), &st) != 0) return "missing";
#ifdef __APPLE__
    const auto mt = st.st_mtimespec;
    const auto ct = st.st_ctimespec;
#else
    const auto mt = st.st_mtim;
    const auto ct = st.st_ctim;
#endif
    const auto stamp =
        std::to_string(st.st_dev) + ":" + std::to_string(st.st_ino) + ":" +
        std::to_string(st.st_mode) + ":" + std::to_string(st.st_size) + ":" +
        std::to_string(mt.tv_sec) + ":" + std::to_string(mt.tv_nsec) + ":" +
        std::to_string(ct.tv_sec) + ":" + std::to_string(ct.tv_nsec);
    auto& saved = hashes_[p.abs()];
    if (saved.first != stamp) saved = {stamp, digest(next_->readFile(p))};
    return saved.second;
  }
  auto observe(char op, const nix::CanonPath& p) -> std::string {
    try {
      switch (op) {
        case 'f':
          return file_digest(p);
        case 's':
          return stat_text(next_->maybeLstat(p));
        case 'd':
          return directory_digest(p, next_->readDirectory(p));
        case 'l':
          return next_->readLink(p);
      }
    } catch (const nix::Error&) {
      return "unreadable";
    }
    return "unknown";
  }
  auto remember(char op, const nix::CanonPath& p, const std::string& value,
                bool filtered = false) -> void {
    if (!reusable) return;
    if (store_path(p)) {
      const auto end = p.abs().find('/', std::string("/nix/store/").size());
      const auto root = p.abs().substr(0, end);
      if (!store_roots.contains(root) &&
          next_->maybeLstat(nix::CanonPath(root))) {
        if (store_roots.size() >= 8192)
          reusable = false;
        else
          store_roots.insert(root);
      }
      if (store_roots.contains(root) && !filtered) return;
    }
    // Bounded auxiliary memory even for a definition enumerating a huge tree.
    // Such evaluations still work, but are not reused.
    if (reads_.size() >= 8192) {
      reusable = false;
      reads_.clear();
      hashes_.clear();
      return;
    }
    auto [it, inserted] = reads_.emplace(Key{op, p.abs()}, value);
    if (!inserted && it->second != value) reusable = false;
  }
};

NixLogSink* g_logger = nullptr;

std::string message(const nix::BaseError& e) {
  return nix::filterANSIEscapes(e.what(), true);
}

void on_usr1(int) {}  // interrupts a blocking read of the daemon connection

bool interrupted(const NixCall& c, bool* timed_out) {
  if (c.cancel.stop_requested()) return true;
  if (c.deadline && std::chrono::steady_clock::now() >= *c.deadline) {
    if (timed_out) *timed_out = true;
    return true;
  }
  return false;
}

auto interruption(const NixCall& call) -> NixInterrupted {
  bool timed_out = false;
  interrupted(call, &timed_out);
  return {timed_out,
          timed_out ? "Nix exceeded buildTimeoutSeconds" : "Nix cancelled"};
}

class WakeOnStop {
 public:
  WakeOnStop(const NixCall& call, std::function<void()> wake)
      : wake_(std::move(wake)), stop_(call.cancel, wake_) {
    if (call.deadline) {
      timer_ =
          std::jthread([this, deadline = *call.deadline](std::stop_token stop) {
            std::unique_lock lock(timer_mu_);
            timer_cv_.wait_until(lock, stop, deadline, [] { return false; });
            if (!stop.stop_requested()) wake_();
          });
    }
  }

 private:
  std::function<void()> wake_;
  std::stop_callback<std::function<void()>> stop_;
  std::mutex timer_mu_;
  std::condition_variable_any timer_cv_;
  std::jthread timer_;
};

class CallScope {
 public:
  explicit CallScope(const NixCall& call,
                     nix::RemoteStore* build_store = nullptr)
      : wake_(call,
              [thread = pthread_self(), build_store] {
                if (build_store) build_store->shutdownConnections();
                pthread_kill(thread, SIGUSR1);
              }),
        log_line_([&call](const std::string& line) {
          call.log({std::span(&line, 1)});
        }) {
    nix::unix::interruptCheck = [&call] { return interrupted(call, nullptr); };
    g_logger->set_thread(call.log ? &log_line_ : nullptr);
    g_logger->set_thread_builds(call.on_build ? &call.on_build : nullptr);
  }

  ~CallScope() {
    g_logger->set_thread(nullptr);
    g_logger->set_thread_builds(nullptr);
    nix::unix::interruptCheck = {};
  }

 private:
  WakeOnStop wake_;
  const std::function<void(const std::string&)> log_line_;
};

// Nix's errors as NixEvaluator promises them.
template <class F>
auto translated(const NixCall& call, F f) -> decltype(f()) {
  if (interrupted(call, nullptr)) throw interruption(call);
  try {
    return f();
  } catch (const nix::Interrupted&) {
    throw interruption(call);
  } catch (const nix::BaseError& e) {
    if (interrupted(call, nullptr)) throw interruption(call);
    throw std::runtime_error(message(e));
  }
}

}  // namespace

void NixEvaluator::init_process() {
  static bool done = false;
  if (done) return;
  done = true;
  NixStore::init_process();  // nix.conf: substituters, the daemon socket, ...
  nix::initGC();
  ::signal(SIGPIPE, SIG_IGN);
  auto logger = std::make_unique<NixLogSink>();
  g_logger = logger.get();
  nix::logger = std::move(logger);
  struct sigaction sa{};
  sa.sa_handler = on_usr1;  // no SA_RESTART: the read returns EINTR and Nix
                            // checks for interrupts
  sigemptyset(&sa.sa_mask);
  ::sigaction(SIGUSR1, &sa, nullptr);
}

auto NixEvaluator::build_client(int argc, char** argv) -> std::optional<int> {
  if (argc < 2 || std::string_view(argv[1]) != "--nix-build-client")
    return std::nullopt;
  const bool sandboxed =
      argc == 6 && std::string_view(argv[5]) == "--sandboxed";
  if (argc != 5 && !sandboxed) return 2;

  try {
    NixStore::init_process();
    if (*argv[3])
      nix::settings.getWorkerSettings().maxBuildJobs.set(argv[3], false);
    if (*argv[4])
      nix::settings.getLocalSettings().buildCores.set(argv[4], false);
    if (sandboxed) {
      nix::settings.set("sandbox", "true");
      nix::settings.set("sandbox-fallback", "false");
      // Only the receiving daemon is bound by these settings.
      nix::settings.set("builders", "");
    }
    nix::logger = std::make_unique<NixLogSink>(
        [sandboxed, truncated = false](const std::string& line) mutable {
          if (sandboxed) {
            const std::string_view text(line);
            write_build_log(text.substr(text.size() > kBuildLogTail
                                            ? text.size() - kBuildLogTail
                                            : 0),
                            false);
            return;
          }
          if (truncated) return;
          truncated = line.size() >= kInvocationLogLimitBytes;
          write_build_log(line, truncated);
        });
    auto store = nix::openStore();
    if (sandboxed && dynamic_cast<nix::RemoteStore*>(&*store) &&
        store->isTrustedClient() != nix::Trusted)
      throw std::runtime_error(
          "the Nix daemon cannot enforce sandbox settings for this client");
    store->buildPaths({nix::DerivedPath::Built{
        .drvPath =
            nix::makeConstantStorePathRef(store->parseStorePath(argv[2])),
        .outputs = nix::OutputsSpec::All{}}});
    return 0;
  } catch (const std::exception& error) {
    auto diagnostic = tsugumi::text::replace_invalid_utf8(
        nix::filterANSIEscapes(error.what(), true));
    nix::logger->log(nix::lvlError, diagnostic);
    // Keep the build diagnostic available even if the history log filled up.
    if (diagnostic.size() < kInvocationLogLimitBytes)
      nix::writeFull(STDERR_FILENO, diagnostic);
    return 1;
  }
}

namespace {

struct SourceFetcher {
  nix::fetchers::Settings settings;
  nix::ref<nix::Store> store = nix::openStore();
  std::map<nix::Hash, nix::StorePath> fetched;

  SourceFetcher() {
    nix::experimentalFeatureSettings.set("extra-experimental-features",
                                         "flakes fetch-tree");
    nix::loadConfFile(settings);
  }

  auto lock(const std::string& url) -> NixSource {
    const auto input = nix::fetchers::Input::fromURL(settings, url);
    const auto [path, locked] = input.fetchToStore(settings, *store);
    const auto hash = store->queryPathInfo(path)->narHash;
    const auto rev = locked.getRev();
    return {url, rev ? rev->to_string(nix::HashFormat::Base16, false) : "",
            hash.to_string(nix::HashFormat::SRI, true)};
  }

  auto materialize(const NixSource& source) -> std::string {
    auto input = nix::fetchers::Input::fromURL(settings, source.url);
    if (!source.rev.empty()) {
      const auto algorithm = source.rev.size() == 64
                                 ? nix::HashAlgorithm::SHA256
                                 : nix::HashAlgorithm::SHA1;
      input = input.applyOverrides(
          std::nullopt,
          nix::Hash::parseNonSRIUnprefixed(source.rev, algorithm));
    }
    auto attrs = input.toAttrs();
    attrs.insert_or_assign("narHash", source.nar_hash);
    return materialize(
        nix::fetchers::Input::fromAttrs(settings, std::move(attrs)));
  }

  auto materialize(const nix::fetchers::Input& input) -> std::string {
    const auto expected = input.getNarHash();
    if (!expected)
      throw std::runtime_error("nixPath source requires narHash");
    if (auto found = fetched.find(*expected); found != fetched.end())
      return store->printStorePath(found->second);
    // A locked fetchTree source is a fixed-output NAR named "source".
    // Reuse a valid object already in the store, including a flake input
    // fetched by the test harness, without a network round trip.
    auto path = store->makeFixedOutputPath(
        "source", {nix::FileIngestionMethod::NixArchive, *expected, {}});
    store->addTempRoot(path);
    if (!store->isValidPath(path))
      path = input.fetchToStore(settings, *store).first;
    store->addTempRoot(path);
    if (store->queryPathInfo(path)->narHash != *expected)
      throw std::runtime_error("nixPath source narHash mismatch");
    fetched.emplace(*expected, path);
    return store->printStorePath(path);
  }
};

// Use Nix's scalar conversion rules while streaming container members into
// the byte budget. The regular Nix JSON conversion builds the entire result
// in memory first, including repeated or recursive lists/attrsets.
auto bounded_json(nix::EvalState& st, nix::Value& value, std::string& out,
                  nix::PosIdx pos = nix::noPos) -> void {
  auto depth = st.addCallDepth(pos);
  nix::checkInterrupt();
  st.forceValue(value, pos);
  const auto append = [&](std::string_view part) {
    if (part.size() > kExpressionBytes - out.size())
      throw std::runtime_error("Nix expression JSON exceeds 8 MiB");
    out += part;
  };
  const auto quoted = [&](std::string_view part) {
    if (part.size() > kExpressionBytes - out.size())
      throw std::runtime_error("Nix expression JSON exceeds 8 MiB");
    append(nlohmann::json(part).dump());
  };
  nix::NixStringContext context;
  if (value.type() == nix::nAttrs) {
    if (auto str = st.tryAttrsToString(pos, value, context, false, false)) {
      quoted(*str);
      return;
    }
    if (auto* path = value.attrs()->get(st.s.outPath)) {
      bounded_json(st, *path->value, out, path->pos);
      return;
    }
    append("{");
    bool first = true;
    for (auto* attr : value.attrs()->lexicographicOrder(st.symbols)) {
      if (!std::exchange(first, false)) append(",");
      quoted(st.symbols[attr->name]);
      append(":");
      bounded_json(st, *attr->value, out, attr->pos);
    }
    append("}");
  } else if (value.type() == nix::nList) {
    append("[");
    bool first = true;
    for (auto* item : value.listView()) {
      if (!std::exchange(first, false)) append(",");
      bounded_json(st, *item, out, pos);
    }
    append("]");
  } else if (value.type() == nix::nString) {
    quoted(value.string_view());
  } else {
    append(nix::printValueAsJSON(st, true, value, pos, context, false).dump());
  }
}

// libnix passes the primop's own position to PrimOp::impl. The evaluation
// hook supplies the actual call site, including the parser's <name> calls.
class CallPositions final : public nix::EvalProfiler {
 public:
  auto preFunctionCallHook(nix::EvalState&, const nix::Value&,
                           std::span<nix::Value*>, nix::PosIdx pos)
      -> void override {
    stack_.push_back(pos);
  }

  auto postFunctionCallHook(nix::EvalState&, const nix::Value&,
                            std::span<nix::Value*>, nix::PosIdx)
      -> void override {
    stack_.pop_back();
  }

  auto current() const -> nix::PosIdx {
    return stack_.empty() ? nix::noPos : stack_.back();
  }

 private:
  auto getNeededHooksImpl() const -> Hooks override {
    Hooks hooks;
    return hooks.set(preFunctionCall).set(postFunctionCall);
  }

  std::vector<nix::PosIdx> stack_;
};

// The pinned libnix has call hooks but no registration method on EvalState.
// An explicit template instantiation may name a private member. Keep access
// in this one translation unit and let a libnix API change fail at compile
// time.
struct ProfilerMember {
  using Pointer = nix::MultiEvalProfiler nix::EvalState::*;
  friend auto profiler_member(ProfilerMember) -> Pointer;
};

template <ProfilerMember::Pointer Member>
struct RegisterProfilerMember {
  friend auto profiler_member(ProfilerMember) -> ProfilerMember::Pointer {
    return Member;
  }
};

template struct RegisterProfilerMember<&nix::EvalState::profiler>;

struct EvaluationState {
  using SearchPath = std::map<std::string, std::string>;
  std::string entry_text;
  std::string system;
  std::vector<std::string> allowed;
  std::vector<NixDefinitionScope> definitions;
  std::map<std::string, SearchPath> projects;
  // Settings outlive each state's own values, positions and caches.
  bool read_only = false;
  std::unique_ptr<nix::EvalSettings> eval_settings;
  nix::fetchers::Settings& fetch_settings;
  nix::ref<nix::Store> store;
  uint64_t& next_epoch;
  uint64_t epoch = 0;
  std::map<std::string, int64_t>& evaluation_times;
  std::optional<nix::PrimOp> path_exists;
  std::map<std::string, std::pair<nix::RootValue, bool>> existence_checks;
  std::vector<std::unique_ptr<nix::PrimOp>> wrapped_ops;
  std::shared_ptr<nix::EvalState> state;
  std::shared_ptr<EvaluationReads> reads;
  std::string expression_text;
  const NixDefinitionScope* expression_scope = nullptr;
  bool capture_derivations = false;
  std::set<std::string> instantiated_derivations;

  EvaluationState(nix::fetchers::Settings& fetch, nix::ref<nix::Store> store,
                  uint64_t& epoch, std::map<std::string, int64_t>& times)
      : fetch_settings(fetch),
        store(std::move(store)),
        next_epoch(epoch),
        evaluation_times(times) {}

  auto origin(nix::EvalState& st, nix::PosIdx pos) const
      -> const NixDefinitionScope* {
    const auto location = st.positions.originOf(pos);
    if (auto file = std::get_if<nix::SourcePath>(&location)) {
      if (file->accessor != st.rootFS) return nullptr;
      for (const auto& definition : definitions) {
        if (file->path.isWithin(nix::CanonPath(definition.directory)))
          return &definition;
      }
    } else if (auto str = std::get_if<nix::Pos::String>(&location);
               str && expression_scope && *str->source == expression_text) {
      return expression_scope;
    }
    return nullptr;
  }

  auto find_file(nix::EvalState& st, nix::PosIdx pos, nix::Value** args,
                 nix::Value& out) -> void {
    st.forceList(*args[0], pos, "while evaluating the Nix search path");
    if (args[0]->listSize() != 0)
      st.error<nix::ThrownError>("Card search paths come from tsugumi.lock")
          .atPos(pos)
          .debugThrow();
    const auto lookup =
        st.forceStringNoCtx(*args[1], pos, "while resolving a Nix search path");
    const auto slash = lookup.find('/');
    const std::string name(lookup.substr(0, slash));
    const auto* owner = origin(st, pos);
    const SearchPath empty;
    const auto& entries = owner ? projects.at(owner->project) : empty;
    const auto found = entries.find(name);
    if (found == entries.end()) {
      // Nix's embedded fetchurl derivation is a language resource, outside
      // NIX_PATH. Locked libraries use it to describe bootstrap downloads.
      // It creates a derivation and cannot download during evaluation.
      const auto location = st.positions.originOf(pos);
      const auto* file = std::get_if<nix::SourcePath>(&location);
      if (!owner && lookup == "nix/fetchurl.nix" && file &&
          file->path.abs().starts_with(std::string(st.store->storeDir) + "/")) {
        out.mkPath(st.findFile(nix::LookupPath{}, lookup, pos), st.mem);
        return;
      }
      std::string declared;
      for (const auto& [entry, _] : entries) {
        if (!declared.empty()) declared += ", ";
        declared += entry;
      }
      st.error<nix::ThrownError>("undeclared nixPath name '%s' (declared: %s)",
                                 name, declared.empty() ? "(none)" : declared)
          .atPos(pos)
          .debugThrow();
    }
    auto path = st.rootPath(nix::CanonPath(found->second));
    if (slash != lookup.npos)
      path = path / nix::CanonPath(lookup.substr(slash));
    path = path.resolveSymlinks();
    if (!path.pathExists())
      st.error<nix::ThrownError>("nixPath '%s' does not contain '%s'", name,
                                 lookup)
          .atPos(pos)
          .debugThrow();
    out.mkPath(path, st.mem);
  }

  auto install(nix::Value& value, nix::PrimOp operation) -> void {
    wrapped_ops.push_back(std::make_unique<nix::PrimOp>(std::move(operation)));
    value.mkPrimOp(wrapped_ops.back().get());
  }

  auto open() -> void {
    epoch = ++next_epoch;
    eval_settings = std::make_unique<nix::EvalSettings>(read_only);
    nix::loadConfFile(*eval_settings);
    eval_settings->restrictEval = true;
    eval_settings->pureEval = false;
    eval_settings->enableImportFromDerivation = false;
    eval_settings->allowedUris = nix::Strings{};
    eval_settings->nixPath = nix::Strings{};
    eval_settings->currentSystem = system;
    state = std::make_shared<nix::EvalState>(nix::LookupPath{}, store,
                                             fetch_settings, *eval_settings);
    auto access =
        state->rootFS.dynamic_pointer_cast<nix::FilteringSourceAccessor>();
    if (!access)
      throw std::runtime_error(
          "restricted evaluator has no filesystem access control");
    reads = std::make_shared<EvaluationReads>(access->next);
    access->next = nix::ref<nix::SourceAccessor>(reads);
    reads->allowed_path = [ptr = access.get()](const nix::CanonPath& path) {
      return ptr->isAllowed(path);
    };
    // currentTime is constant across filesystem invalidation for this system.
    auto& time = state->getBuiltin("currentTime");
    auto [saved, _] = evaluation_times.try_emplace(
        system,
        state->forceInt(time, nix::noPos, "while reading evaluator time")
            .value);
    time.mkInt(saved->second);

    // Fetch primops bypass the filesystem allow-list. Refuse them before
    // forcing any of their arguments, even for a hash already in the store.
    for (const char* name :
         {"fetchTarball", "fetchurl", "fetchGit", "fetchTree", "fetchMercurial",
          "fetchClosure", "getFlake", "storePath", "exec", "importNative"}) {
      auto* attr =
          state->getBuiltins().attrs()->get(state->symbols.create(name));
      if (!attr || attr->value->type() != nix::nFunction ||
          !attr->value->isPrimOp())
        continue;
      auto wrapped = *attr->value->primOp();
      wrapped.impl = [name](nix::EvalState& st, nix::PosIdx pos, nix::Value**,
                            nix::Value&) {
        st.error<nix::ThrownError>("%s is forbidden during Card evaluation",
                                   name)
            .atPos(pos)
            .debugThrow();
      };
      install(*attr->value, std::move(wrapped));
    }
    auto& find = state->getBuiltin("findFile");
    auto lookup = *find.primOp();
    auto positions = nix::make_ref<CallPositions>();
    ((*state).*profiler_member(ProfilerMember{})).addProfiler(positions);
    lookup.impl = [this, positions](nix::EvalState& st, nix::PosIdx,
                                    nix::Value** args, nix::Value& out) {
      find_file(st, positions->current(), args, out);
    };
    install(find, std::move(lookup));
    // A caller can construct an attrset with type = "derivation", but cannot
    // use it to ask the tool to read an unrelated store object's files.
    auto& strict = state->getBuiltin("derivationStrict");
    auto strict_op = *strict.primOp();
    auto instantiate = strict_op;
    instantiate.impl = [this, strict_op](nix::EvalState& st, nix::PosIdx pos,
                                         nix::Value** args, nix::Value& out) {
      strict_op.impl(st, pos, args, out);
      if (capture_derivations)
        instantiated_derivations.emplace(
            out.attrs()->get(st.s.drvPath)->value->string_view());
    };
    install(strict, std::move(instantiate));
    auto& path = state->getBuiltin("path");
    auto path_op = *path.primOp();
    auto add_path = path_op;
    add_path.impl = [path_op](nix::EvalState& st, nix::PosIdx pos,
                              nix::Value** args, nix::Value& out) {
      st.forceAttrs(*args[0], pos, "while checking builtins.path");
      if (args[0]->attrs()->get(st.symbols.create("sha256")))
        st.error<nix::ThrownError>(
              "builtins.path with sha256 is forbidden during Card evaluation")
            .atPos(pos)
            .debugThrow();
      path_op.impl(st, pos, args, out);
    };
    install(path, std::move(add_path));
    // A denied pathExists is answered by the allow-list without reaching its
    // underlying accessor. Replay these probes too: later evaluations can add
    // allowed store paths. Otherwise a negative answer could become stale.
    auto& exists = state->getBuiltin("pathExists");
    path_exists = *exists.primOp();
    auto tracked_exists = *path_exists;
    tracked_exists.impl = [this](nix::EvalState& st, nix::PosIdx pos,
                                 nix::Value** args, nix::Value& out) {
      path_exists->impl(st, pos, args, out);
      // Successful lookups already reached the accessor. Only a negative
      // answer can have been hidden by the allow-list.
      if (out.boolean()) return;
      nix::NixStringContext context;
      const auto path = st.coerceToPath(
          pos, *args[0], context, "while recording a path existence check");
      const bool directory = args[0]->type() == nix::nString &&
                             (args[0]->string_view().ends_with("/") ||
                              args[0]->string_view().ends_with("/."));
      const auto key = std::to_string(path.accessor->number) + ":" +
                       path.path.abs() + (directory ? "/" : "");
      if (existence_checks.size() >= 8192)
        reads->reusable = false;
      else
        existence_checks.try_emplace(key, nix::allocRootValue(args[0]),
                                     out.boolean());
    };
    install(exists, std::move(tracked_exists));
    for (const auto& p : allowed) state->allowPathLegacy(p);
    for (const auto& def : definitions) state->allowPathLegacy(def.directory);
    for (const auto& [_, entries] : projects)
      for (const auto& [__, path] : entries) state->allowPathLegacy(path);
  }

  auto unchanged() -> bool {
    if (!reads->unchanged()) return false;
    for (const auto& [_, check] : existence_checks) {
      nix::Value* args[] = {*check.first};
      nix::Value out;
      path_exists->impl(*state, nix::noPos, args, out);
      if (out.boolean() != check.second) return false;
    }
    return reads->reusable;
  }

  auto reset() -> void {
    existence_checks.clear();
    state.reset();
    wrapped_ops.clear();
  }

  auto begin() -> void {
    reset();
    open();
  }

  auto arguments(const NixCall& call, nix::Value& out) -> void {
    nix::Value entry;
    auto& args = *state->allocValue();
    const auto root = state->rootPath(nix::CanonPath("/"));
    state->eval(state->parseExprFromString(entry_text, root), entry);
    state->eval(state->parseExprFromString(call.args, root), args);
    state->callFunction(entry, args, out, nix::noPos);
  }

  auto derivation(nix::EvalState& st, nix::Value& value,
                  const std::string& system) -> WorkerDerivation {
    st.forceValue(value, nix::noPos);
    const auto drv = nix::getDerivation(st, value, false);
    if (!drv)
      throw std::runtime_error("Nix expression must return a derivation");
    const auto out = drv->queryOutPath();
    const auto drv_path = drv->requireDrvPath();
    store->addTempRoot(drv_path);
    return {store->printStorePath(drv_path), store->printStorePath(out),
            system};
  }

  auto plan(const NixCall& c, nix::Value& value, SourceFetcher& fetch)
      -> WorkerDerivation {
    nix::NixStringContext context;
    const auto text = state->forceString(
        value, context, nix::noPos, "while reading the Card execution plan");
    const auto pkgs = fetch.materialize(c.core_nixpkgs);
    // Use a separate state for the core's own package set and worker.
    // Transfer the plan as data with Nix string context, never as code.
    bool read_only = false;
    nix::EvalSettings settings(read_only);
    settings.currentSystem = c.system;
    settings.enableImportFromDerivation = false;
    settings.restrictEval = true;
    settings.allowedUris = nix::Strings{};
    settings.nixPath = nix::Strings{};
    nix::EvalState glue(nix::LookupPath{}, store, fetch_settings, settings);
    glue.allowPathLegacy(pkgs);
    if (c.needs_worker) glue.allowPathLegacy(c.nix_dir.string());
    auto quote = [](const std::string& s) {
      std::string result = "\"";
      for (char ch : s) {
        if (ch == '"' || ch == '\\' || ch == '$') result += '\\';
        result += ch;
      }
      return result + "\"";
    };
    const auto expr =
        "text: let pkgs = import (/. + " + quote(pkgs) +
        ") { config = {}; overlays = []; }; in " +
        (c.needs_worker
             ? "let worker = import (/. + " +
                   quote((c.nix_dir / "worker/worker.nix").string()) +
                   ") { inherit pkgs; }; in "
                   "pkgs.writeText \"tsugumi-card-plan.json\" "
                   "(builtins.appendContext (builtins.toJSON "
                   "(builtins.fromJSON (builtins.unsafeDiscardStringContext "
                   "text) // "
                   "{ worker = \"${worker}/bin/tsugumi-worker\"; })) "
                   "(builtins.getContext text))"
             : "pkgs.writeText \"tsugumi-card-plan.json\" text");
    nix::Value function, arg, output;
    glue.eval(
        glue.parseExprFromString(expr, glue.rootPath(nix::CanonPath("/"))),
        function);
    arg.mkString(text, context, glue.mem);
    glue.callFunction(function, arg, output, nix::noPos);
    return derivation(glue, output, c.system);
  }

  auto evaluate(const NixCall& c, SourceFetcher& fetch) -> NixResult {
    begin();
    nix::Value result;
    arguments(c, result);
    auto* vres = &result;
    state->forceAttrs(*vres, nix::noPos,
                      "while evaluating the Card execution plan");
    auto attr = [&](const char* name) -> nix::Value& {
      return *state
                  ->getAttr(state->symbols.create(name), vres->attrs(),
                            "while reading the Card execution plan")
                  ->value;
    };
    NixResult r;
    r.execution_list = state->forceBool(attr("executionList"), nix::noPos,
                                        "while reading the Card run shape");
    const auto count = state
                           ->forceInt(attr("implementationCount"), nix::noPos,
                                      "while counting Card implementations")
                           .value;
    enforce_implementation_limit(static_cast<uint64_t>(count),
                                 c.max_implementations);
    r.implementation_count = static_cast<uint32_t>(count);
    const auto labels = nlohmann::json::parse(
        state->forceStringNoCtx(attr("implementationLabels"), nix::noPos,
                                "while reading the run list labels"));
    for (const auto& label : labels)
      r.implementation_labels.push_back(label.get<std::string>());
    check_implementation_labels(r.implementation_labels, count);
    r.derivation = plan(c, attr("plan"), fetch);
    if (store->readDerivation(store->parseStorePath(r.derivation.path))
            .platform != c.system)
      throw std::runtime_error("Card derivation does not target system " +
                               c.system);
    if (reads->reusable) {
      for (const auto& path : reads->store_roots) {
        const auto source = store->parseStorePath(path);
        store->addTempRoot(source);
        if (!store->isValidPath(source)) reads->reusable = false;
      }
    }
    r.evaluation_epoch = unchanged() ? epoch : 0;
    return r;
  }

  auto expression(const NixCall& call, const std::string& text, bool build)
      -> E::ExpressionValueT {
    begin();
    const auto def =
        std::ranges::find(definitions, call.card, &NixDefinitionScope::card);
    if (def == definitions.end())
      throw std::runtime_error("missing Card scope");
    expression_scope = &*def;
    expression_text = "{ params, inputs }: (\n" + text + "\n)";
    capture_derivations = build;
    Finally clear_expression([&] {
      expression_scope = nullptr;
      expression_text.clear();
      capture_derivations = false;
      instantiated_derivations.clear();
    });
    nix::Value function, args, value;
    const auto dir =
        state->rootPath(nix::CanonPath(call.definition_dir.string()));
    state->eval(state->parseExprFromString(expression_text, dir), function);
    arguments(call, args);
    state->callFunction(function, args, value, nix::noPos);
    E::ExpressionValueT result;
    if (build) {
      state->forceValue(value, nix::noPos);
      const auto drv = nix::getDerivation(*state, value, false);
      if (!drv)
        throw std::runtime_error("Nix expression must return a derivation");
      const auto path = drv->requireDrvPath();
      result.drv = store->printStorePath(path);
      if (!instantiated_derivations.contains(result.drv))
        throw std::runtime_error(
            "Nix expression must instantiate its derivation");
      result.output_name = drv->queryOutputName();
      if (!store->readDerivation(path).outputs.contains(result.output_name))
        throw std::runtime_error(
            "Nix expression returned an unknown derivation output");
      store->addTempRoot(path);
    } else {
      bounded_json(*state, value, result.json);
    }
    return result;
  }

  // The Nix client sends these options with its own daemon connection. The
  // evaluator and the other builds can keep using their own settings.
  static auto build(const NixCall& c, const std::string& drv,
                    bool sandboxed = false) -> void {
    const auto executable = nix::getSelfExe();
    if (!executable)
      throw std::runtime_error("cannot locate the Nix build client");
    proc::Spec spec;
    spec.argv = {executable->string(), "--nix-build-client", drv,
                 c.jobs ? std::to_string(*c.jobs) : "",
                 c.cores ? std::to_string(*c.cores) : ""};
    if (sandboxed) spec.argv.emplace_back("--sandboxed");
    // Use the same store, Nix configuration and environment as the evaluator.
    for (const auto& [key, value] : nix::getEnv())
      spec.env.push_back(key + "=" + value);

    // Stop ends the client's process group.
    std::atomic<bool> stopped{false};
    std::stop_callback on_stop(c.cancel, [&] { stopped = true; });
    const auto result = proc::run(
        spec, &stopped, c.deadline,
        [&](proc::LineChunk chunk) {
          if (!c.log) return;
          std::vector<std::string> output;
          for (const auto& frame : chunk.lines) {
            const auto* start = reinterpret_cast<const uint8_t*>(frame.data());
            flatbuffers::Verifier verifier(start, frame.size());
            if (!verifier.VerifySizePrefixedBuffer<E::BuildLog>(nullptr))
              throw std::runtime_error("invalid Nix build log frame");
            const auto* log =
                flatbuffers::GetSizePrefixedRoot<E::BuildLog>(start);
            if (log->truncated()) chunk.truncated = true;
            if (log->line()) output.push_back(log->line()->str());
          }
          c.log({output, chunk.truncated});
        },
        kInvocationLogLimitBytes + 64, std::chrono::seconds(1),
        proc::LogFraming::stdout_size_prefixed);
    if (result.cancelled) throw NixInterrupted(false, "Nix cancelled");
    if (result.timed_out)
      throw NixInterrupted(true, "Nix exceeded buildTimeoutSeconds");
    if (!proc::exited_ok(result.status))
      throw std::runtime_error(
          result.out.empty()
              ? "Nix build failed: " + proc::describe(result.status)
              : tsugumi::text::replace_invalid_utf8(result.out));
  }
  static auto realise_native(const NixCall& c, NixResult r) -> NixResult {
    auto store = nix::openStore();
    CallScope scope(c, dynamic_cast<nix::RemoteStore*>(&*store));
    const auto out = store->parseStorePath(r.derivation.output);
    // Held by this connection from here: a concurrent GC never removes it
    // between the check, the build and the permanent root. The core's
    // evaluation roots connection holds the derivation.
    store->addTempRoot(out);
    // The daemon connection is now established even if Stop raced its setup.
    nix::checkInterrupt();
    if (!store->isValidPath(out)) {
      if (c.on_phase) c.on_phase(true);
      nix::checkInterrupt();
      build(c, r.derivation.path);
      r.built = true;
    }
    nix::checkInterrupt();
    nix::StorePathSet closure;
    r.closure.clear();
    store->computeFSClosure(out, closure);
    for (const auto& path : closure)
      r.closure.push_back(store->printStorePath(path));
    std::sort(r.closure.begin(), r.closure.end());
    if (!c.root_dir.empty()) {
      add_root(*store, r.derivation.output,
               c.root_dir / (c.root_prefix + std::string(out.to_string())));
    }
    std::ifstream f(r.derivation.output, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    r.plan_json = ss.str();
    return r;
  }

  // The project's permanent root: a symlink, recreated on every use (its age
  // tells when the result was last needed).
  static void add_root(nix::Store& store, const std::string& out,
                       const std::filesystem::path& link) {
    static std::atomic<uint64_t> serial{0};
    std::filesystem::create_directories(link.parent_path());
    auto tmp = link;
    tmp +=
        ".tmp-" + std::to_string(::getpid()) + "-" + std::to_string(++serial);
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    std::filesystem::create_symlink(out, tmp);
    std::filesystem::rename(tmp, link);
    std::string l = link.string();
    if (!l.starts_with("/nix/var/nix/gcroots/")) {
      // Outside the GC root folder (a project on a development host): the
      // daemon records it as an indirect root.
      if (auto* ir = dynamic_cast<nix::IndirectRootStore*>(&store))
        ir->addIndirectRoot(link);
    }
  }
  auto reuse(const NixResult& result) -> bool {
    if (!state || !result.evaluation_epoch || result.evaluation_epoch != epoch)
      return false;
    if (!unchanged()) {
      reset();
      return false;
    }
    auto drv = store->parseStorePath(result.derivation.path);
    auto out = store->parseStorePath(result.derivation.output);
    store->addTempRoot(drv);
    store->addTempRoot(out);
    if (store->isValidPath(drv) && store->isValidPath(out)) return true;
    // Nix remembers derivations it may not write a second time after GC.
    reset();
    return false;
  }
};

template <class Table>
auto receive(nix::Source& source) -> typename Table::NativeTableType {
  const auto bytes = nix::readString(source);
  flatbuffers::Verifier verifier(reinterpret_cast<const uint8_t*>(bytes.data()),
                                 bytes.size());
  if (!verifier.VerifyBuffer<Table>(nullptr))
    throw std::runtime_error("invalid Nix evaluator message");
  typename Table::NativeTableType result;
  flatbuffers::GetRoot<Table>(bytes.data())->UnPackTo(&result);
  return result;
}

template <class Table, class Message>
auto send(nix::FdSink& sink, Message&& message, std::string system = {})
    -> void {
  typename Table::NativeTableType envelope;
  if constexpr (std::is_same_v<Table, E::Request>)
    envelope.system = std::move(system);
  envelope.body.Set(std::forward<Message>(message));
  flatbuffers::FlatBufferBuilder builder;
  builder.Finish(Table::Pack(builder, &envelope));
  sink << std::string_view(
      reinterpret_cast<const char*>(builder.GetBufferPointer()),
      builder.GetSize());
  sink.flush();
}

auto pack_source(const std::string& name, const NixSource& source)
    -> Tsugumi::Document::LockEntryT {
  Tsugumi::Document::LockEntryT result;
  result.name = name;
  result.url = source.url;
  result.rev = source.rev;
  result.narHash = source.nar_hash;
  return result;
}

auto unpack_source(const Tsugumi::Document::LockEntryT& source) -> NixSource {
  return {source.url, source.rev, source.narHash};
}

auto pack_scope(const NixCall& call) -> std::unique_ptr<E::ScopeT> {
  auto result = std::make_unique<E::ScopeT>();
  result->generated = call.generated;
  for (const auto& definition : call.definitions) {
    auto entry = std::make_unique<E::DefinitionScopeT>();
    entry->directory = definition.directory;
    entry->card = definition.card;
    entry->project = definition.project;
    result->definitions.push_back(std::move(entry));
  }
  for (const auto& [project, entries] : call.projects) {
    auto scope = std::make_unique<E::ProjectScopeT>();
    scope->project = project;
    for (const auto& [name, source] : entries)
      scope->entries.push_back(std::make_unique<Tsugumi::Document::LockEntryT>(
          pack_source(name, source)));
    result->projects.push_back(std::move(scope));
  }
  return result;
}

auto unpack_scope(const E::ScopeT& data, NixCall& call) -> void {
  call.generated = data.generated;
  for (const auto& def : data.definitions)
    call.definitions.push_back({def->directory, def->card, def->project});
  for (const auto& project : data.projects) {
    auto& entries = call.projects[project->project];
    for (const auto& source : project->entries)
      entries.emplace(source->name, unpack_source(*source));
  }
}

constexpr int kEvaluatorSocket = 3;
constexpr int kCoreLifetime = 4;

auto executable_path() -> std::filesystem::path {
#ifdef __APPLE__
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string path(size, '\0');
  _NSGetExecutablePath(path.data(), &size);
  return std::filesystem::canonical(path.c_str());
#else
  return std::filesystem::read_symlink("/proc/self/exe");
#endif
}

auto evaluator_main() -> void {
  nix::FdSource source(kEvaluatorSocket);
  nix::FdSink sink(kEvaluatorSocket);
  auto init = receive<E::Request>(source);
  const auto* settings = init.body.AsInitialize();
  if (!settings) throw std::runtime_error("missing Nix evaluator settings");
  SourceFetcher fetch;
  uint64_t epoch = settings->epoch - 1;
  std::map<std::string, int64_t> times;
  using ScopeKey = std::tuple<std::string, std::vector<NixDefinitionScope>,
                              std::vector<std::string>,
                              std::map<std::string, LockedNixPath>>;
  std::list<std::pair<ScopeKey, std::unique_ptr<EvaluationState>>> warm;
  auto evaluator = [&](const NixCall& call) -> EvaluationState& {
    auto key =
        ScopeKey{call.system, call.definitions, call.generated, call.projects};
    auto cached =
        std::ranges::find(warm, key, &decltype(warm)::value_type::first);
    if (cached != warm.end()) {
      warm.splice(warm.begin(), warm, cached);
      return *warm.front().second;
    }
    auto eval = std::make_unique<EvaluationState>(fetch.settings, fetch.store,
                                                  epoch, times);
    eval->entry_text = settings->entry;
    eval->system = call.system;
    eval->allowed = call.generated;
    eval->definitions = call.definitions;
    // Fetch (and verify) everything before opening the restricted state.
    for (const auto& [project, entries] : call.projects) {
      auto& paths = eval->projects[project];
      for (const auto& [name, source] : entries)
        paths.emplace(name, fetch.materialize(source));
    }
    if (warm.size() >= 4) warm.pop_back();
    warm.emplace_front(std::move(key), std::move(eval));
    return *warm.front().second;
  };
  std::map<std::pair<std::string, std::string>, std::string> params;
  auto evaluate = [&](const E::RequestT& request) {
    NixCall call;
    call.system = request.system;
    const std::function<void(const std::string&)> log =
        [&](const std::string& line) {
          E::LogT log;
          log.line = line;
          send<E::Reply>(sink, std::move(log));
        };
    const NixLogSink::BuildActivity builds = [&](const std::string& drv,
                                                 bool active) {
      E::BuildT build;
      build.drv = drv;
      build.active = active;
      send<E::Reply>(sink, std::move(build));
    };
    g_logger->set_thread(&log);
    g_logger->set_thread_builds(&builds);
    Finally reset_logger([] {
      g_logger->set_thread(nullptr);
      g_logger->set_thread_builds(nullptr);
    });
    if (const auto* op = request.body.AsEvaluate()) {
      unpack_scope(*op->scope, call);
      call.core_nixpkgs = unpack_source(*op->core_nixpkgs);
      call.nix_dir = op->nix_dir;
      call.needs_worker = op->needs_worker;
      call.args = op->args;
      call.max_implementations = op->max_implementations;
      const auto result = evaluator(call).evaluate(call, fetch);
      E::EvaluationT reply;
      reply.drv = result.derivation.path;
      reply.out = result.derivation.output;
      reply.execution_list = result.execution_list;
      reply.implementation_count = result.implementation_count;
      reply.implementation_labels = result.implementation_labels;
      reply.epoch = result.evaluation_epoch;
      send<E::Reply>(sink, std::move(reply));
    } else if (const auto* op = request.body.AsLockSource()) {
      const auto source = fetch.lock(op->url);
      E::ResolvedSourceT locked;
      locked.rev = source.rev;
      locked.narHash = source.nar_hash;
      send<E::Reply>(sink, std::move(locked));
    } else if (const auto* op = request.body.AsExpression()) {
      unpack_scope(*op->scope, call);
      call.args = op->args;
      call.card = op->card;
      call.definition_dir = op->definition_dir;
      auto& eval = evaluator(call);
      auto result = eval.expression(call, op->text, op->build);
      send<E::Reply>(sink, std::move(result));
    } else if (const auto* op = request.body.AsParamsDeclaration()) {
      const auto hash = nix::hashString(nix::HashAlgorithm::SHA256, op->text);
      const auto key = std::pair{
          call.system, hash.to_string(nix::HashFormat::Base16, false)};
      auto found = params.find(key);
      if (found == params.end()) {
        bool read_only = true;
        nix::EvalSettings settings(read_only);
        settings.pureEval = true;
        settings.restrictEval = true;
        settings.enableImportFromDerivation = false;
        settings.allowedUris = nix::Strings{};
        settings.nixPath = nix::Strings{};
        settings.currentSystem = call.system;
        nix::EvalState state(nix::LookupPath{}, fetch.store, fetch.settings,
                             settings);
        auto declaration = read_params_nix(state, op->text, op->origin);
        if (declaration.size() > kExpressionBytes)
          throw std::runtime_error("params.nix declarations exceed 8 MiB");
        if (params.size() >= 256) params.erase(params.begin());
        found = params.emplace(key, std::move(declaration)).first;
      }
      E::ParamsValueT result;
      result.json = found->second;
      send<E::Reply>(sink, std::move(result));
    } else if (const auto* op = request.body.AsReuse()) {
      NixResult result;
      result.evaluation_epoch = op->epoch;
      result.derivation.path = op->drv;
      result.derivation.output = op->out;
      E::ReusableT reply;
      unpack_scope(*op->scope, call);
      reply.value = evaluator(call).reuse(result);
      send<E::Reply>(sink, std::move(reply));
    } else {
      throw std::runtime_error("unknown Nix evaluator operation");
    }
  };
  auto send_error = [&](std::string text, bool limit) {
    E::ErrorT reply;
    reply.message = std::move(text);
    reply.limit = limit;
    send<E::Reply>(sink, std::move(reply));
  };
  for (;;) {
    E::RequestT request;
    try {
      request = receive<E::Request>(source);
    } catch (const nix::EndOfFile&) {
      return;
    }
    try {
      evaluate(request);
    } catch (const NixImplementationLimit& e) {
      send_error(e.what(), true);
    } catch (const nix::BaseError& e) {
      send_error(message(e), false);
    } catch (const std::exception& e) {
      send_error(e.what(), false);
    }
  }
}

}  // namespace

// One process keeps the evaluator and its cache warm. The front waiter owns
// the connection. Cancelling a queued caller removes only that waiter.
struct NixEvaluator::Impl {
  std::string entry;
  std::mutex mu;
  std::condition_variable_any changed;
  std::list<int> waiters;
  nix::AutoCloseFD socket, lifetime;
  pid_t pid = -1;
  uint64_t last_epoch = 0;
  // Evaluated derivations can still be in use when a later evaluation is
  // stopped. The core holds their roots across evaluator process lifetimes.
  std::shared_ptr<nix::Store> roots;

  ~Impl() { close(); }

  auto close() -> void {
    if (pid < 0) return;
    proc::kill_group(pid, SIGKILL);
    proc::wait(pid);
    pid = -1;
    socket = nix::AutoCloseFD{};
    lifetime = nix::AutoCloseFD{};
  }

  auto open() -> void {
    int pair[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
      throw std::runtime_error("cannot open Nix evaluator socket");
    nix::AutoCloseFD parent(pair[0]), child(pair[1]);
    nix::unix::closeOnExec(parent.get());
    nix::unix::closeOnExec(child.get());
    int pipe[2];
    if (::pipe(pipe) != 0)
      throw std::runtime_error("cannot watch the core's lifetime");
    nix::AutoCloseFD reader(pipe[0]), writer(pipe[1]);
    nix::unix::closeOnExec(reader.get());
    nix::unix::closeOnExec(writer.get());

    proc::Spec spec;
    spec.argv = {executable_path(), "--nix-evaluator"};
    for (auto e = environ; *e; ++e)
      if (!std::string_view(*e).starts_with("NIX_PATH="))
        spec.env.emplace_back(*e);
    spec.stderr_fd = STDERR_FILENO;
    spec.fds.emplace_back(kEvaluatorSocket, child.get());
    spec.fds.emplace_back(kCoreLifetime, reader.get());
    pid = proc::spawn(spec);
    lifetime = std::move(writer);
    socket = std::move(parent);
  }

  auto hold(std::string_view path) -> void {
    if (!roots) roots = nix::openStore().get_ptr();
    roots->addTempRoot(roots->parseStorePath(path));
  }

  template <class Request, class Result>
  auto exchange(const NixCall& call, Request request, Result result)
      -> decltype(result(std::declval<E::ReplyBodyUnion&>())) {
    std::unique_lock lock(mu);
    const auto waiter = waiters.emplace(waiters.end());
    Finally dequeue([&] {
      if (!lock.owns_lock()) lock.lock();
      waiters.erase(waiter);
      changed.notify_all();
    });
    lock.unlock();
    if (call.on_phase) call.on_phase(false);
    lock.lock();
    auto ready = [&] { return waiter == waiters.begin(); };
    if (call.deadline)
      changed.wait_until(lock, call.cancel, *call.deadline, ready);
    else
      changed.wait(lock, call.cancel, ready);
    if (interrupted(call, nullptr)) throw interruption(call);
    lock.unlock();

    E::ReplyT reply;
    try {
      const bool start = pid < 0;
      if (start) open();
      WakeOnStop wake(call, [pid = pid] { proc::kill_group(pid, SIGKILL); });
      if (interrupted(call, nullptr)) throw interruption(call);
      nix::FdSink sink(socket.get());
      nix::FdSource source(socket.get());
      if (start) {
        E::InitializeT settings;
        settings.entry = entry;
        settings.epoch = last_epoch + 1;
        send<E::Request>(sink, std::move(settings));
      }
      send<E::Request>(sink, std::move(request), call.system);
      for (;;) {
        reply = receive<E::Reply>(source);
        if (const auto* build = reply.body.AsBuild()) {
          if (call.on_build) call.on_build(build->drv, build->active);
          continue;
        }
        const auto* log = reply.body.AsLog();
        if (!log) break;
        if (call.log) call.log({std::span(&log->line, 1)});
      }
    } catch (const std::exception& e) {
      close();
      if (interrupted(call, nullptr)) throw interruption(call);
      throw std::runtime_error("Nix evaluator connection failed: " +
                               std::string(e.what()));
    }
    if (interrupted(call, nullptr)) {
      close();
      throw interruption(call);
    }
    if (const auto* error = reply.body.AsError()) {
      if (error->limit) throw NixImplementationLimit(error->message);
      throw std::runtime_error(error->message);
    }
    CallScope scope(call);
    return translated(call, [&] { return result(reply.body); });
  }
};

auto NixEvaluator::run_subprocess_if_requested(int argc, char** argv) -> void {
  if (argc != 2 || std::string_view(argv[1]) != "--nix-evaluator") return;
  nix::unix::closeOnExec(kEvaluatorSocket);
  nix::unix::closeOnExec(kCoreLifetime);
  init_process();
  // The Nix headers register the thread with Boehm GC. Its larger stack
  // accommodates deeply nested nixpkgs calls.
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 64u << 20);
  pthread_t thread;
  const int rc = pthread_create(
      &thread, &attr,
      [](void*) -> void* {
        try {
          evaluator_main();
          std::_Exit(0);
        } catch (const std::exception& e) {
          std::fprintf(stderr, "Nix evaluator: %s\n", e.what());
          std::_Exit(1);
        }
      },
      nullptr);
  pthread_attr_destroy(&attr);
  if (rc != 0) throw std::runtime_error("cannot start the Nix evaluator");
  // A core crash closes its end even while evaluation is blocked in libnix.
  char unused;
  while (::read(kCoreLifetime, &unused, sizeof(unused)) < 0 && errno == EINTR) {
  }
  proc::kill_group(::getpid(), SIGKILL);
  std::_Exit(1);
}

NixEvaluator::NixEvaluator(std::string entry)
    : impl_(std::make_unique<Impl>()) {
  impl_->entry = std::move(entry);
}

NixEvaluator::~NixEvaluator() = default;

auto NixEvaluator::evaluate(const NixCall& call) -> NixResult {
  if (call.system.empty())
    throw std::runtime_error("execution target system is unknown");
  E::EvaluateT request;
  request.scope = pack_scope(call);
  request.core_nixpkgs =
      std::make_unique<Tsugumi::Document::LockEntryT>(
          pack_source("nixpkgs", call.core_nixpkgs));
  request.nix_dir = call.nix_dir.string();
  request.needs_worker = call.needs_worker;
  request.args = call.args;
  request.max_implementations = call.max_implementations;
  return impl_->exchange(call, std::move(request), [&](auto& reply) {
    const auto* data = reply.AsEvaluation();
    if (!data) throw std::runtime_error("missing Nix evaluation reply");
    NixResult result;
    result.derivation = {data->drv, data->out, call.system};
    result.execution_list = data->execution_list;
    result.implementation_count = data->implementation_count;
    result.implementation_labels = data->implementation_labels;
    result.evaluation_epoch = data->epoch;
    impl_->last_epoch = std::max(impl_->last_epoch, result.evaluation_epoch);
    impl_->hold(result.derivation.path);
    return result;
  });
}

auto NixEvaluator::reuse(const NixCall& call, const NixResult& result) -> bool {
  E::ReuseT request;
  request.scope = pack_scope(call);
  request.epoch = result.evaluation_epoch;
  request.drv = result.derivation.path;
  request.out = result.derivation.output;
  return impl_->exchange(call, std::move(request), [&](auto& reply) {
    const auto* data = reply.AsReusable();
    if (!data) throw std::runtime_error("missing Nix reuse reply");
    if (data->value) {
      impl_->hold(result.derivation.path);
      impl_->hold(result.derivation.output);
    }
    return data->value;
  });
}

auto NixEvaluator::lock_source(const std::string& url) -> NixSource {
  E::LockSourceT request;
  request.url = url;
  return impl_->exchange(NixCall{}, std::move(request), [&](auto& reply) {
    const auto* source = reply.AsResolvedSource();
    if (!source) throw std::runtime_error("missing nixPath lock reply");
    return NixSource{url, source->rev, source->narHash};
  });
}

auto NixEvaluator::params_declaration(const NixCall& call,
                                      std::string_view text,
                                      const std::string& origin)
    -> std::string {
  E::ParamsDeclarationT request;
  request.text = text;
  request.origin = origin;
  return impl_->exchange(call, std::move(request), [](auto& reply) {
    auto* data = reply.AsParamsValue();
    if (!data) throw std::runtime_error("missing params.nix reply");
    return std::move(data->json);
  });
}

auto NixEvaluator::expression(const NixCall& call,
                              const std::string& expression, bool build)
    -> ExpressionResult {
  E::ExpressionT request;
  request.scope = pack_scope(call);
  request.args = call.args;
  request.card = call.card;
  request.definition_dir = call.definition_dir.string();
  request.text = expression;
  request.build = build;
  const auto value =
      impl_->exchange(call, std::move(request), [&](auto& reply) {
        auto* data = reply.AsExpressionValue();
        if (!data) throw std::runtime_error("missing Nix expression reply");
        if (build) impl_->hold(data->drv);
        return std::move(*data);
      });
  ExpressionResult result;
  if (!build) {
    result.json = value.json;
    return result;
  }

  auto building = call;
  auto tail = [&](std::string_view text) {
    text = text.substr(text.size() > kBuildLogTail ? text.size() - kBuildLogTail
                                                   : 0);
    if (text.size() + result.log_tail.size() > kBuildLogTail)
      result.log_tail.erase(
          0, text.size() + result.log_tail.size() - kBuildLogTail);
    result.log_tail.append(text);
  };
  building.log = [&](proc::LineChunk chunk) {
    for (const auto& line : chunk.lines) {
      tail(line);
      tail("\n");
    }
    if (call.log) call.log(chunk);
  };
  namespace fsys = std::filesystem;
  auto store = nix::openStore();
  translated(building, [&] {
    CallScope scope(building, dynamic_cast<nix::RemoteStore*>(&*store));
    const auto drv = store->parseStorePath(value.drv);
    const auto output = [&]() -> std::optional<nix::StorePath> {
      const auto outputs = store->queryPartialDerivationOutputMap(drv);
      const auto found = outputs.find(value.output_name);
      if (found == outputs.end() || !found->second) return std::nullopt;
      store->addTempRoot(*found->second);
      if (!store->isValidPath(*found->second)) return std::nullopt;
      return found->second;
    };
    auto out = output();
    try {
      if (!out) {
        EvaluationState::build(building, value.drv, true);
        out = output();
      }
      if (!out)
        throw std::runtime_error(
            "Nix build did not realize the requested output");
    } catch (const NixInterrupted&) {
      throw;
    } catch (const std::exception& error) {
      result.build_error = error.what();
      if (!result.log_tail.ends_with(error.what())) tail(error.what());
      return;
    }
    result.log_tail.clear();
    const fsys::path path(store->printStorePath(*out));
    size_t bytes = 0;
    auto add = [&](const fsys::path& root, const fsys::path& relative,
                   const std::string& name) {
      const auto size = fsys::file_size(root / relative);
      if (name.size() > kExpressionBytes - bytes ||
          size > kExpressionBytes - bytes - name.size()) {
        result.truncated = true;
        return false;
      }
      ExpressionFile file{name, {}};
      file.bytes.reserve(size);
      bytes += name.size() + fs::stream_beneath(root, relative, size,
                                                [&](std::string_view chunk) {
                                                  nix::checkInterrupt();
                                                  file.bytes += chunk;
                                                });
      result.files.push_back(std::move(file));
      return true;
    };
    const auto status = fsys::symlink_status(path);
    if (fsys::is_regular_file(status)) {
      add(path.parent_path(), path.filename(), path.filename().string());
    } else if (fsys::is_directory(status)) {
      size_t entries = 0;
      for (const auto& item : fsys::recursive_directory_iterator(path)) {
        nix::checkInterrupt();
        if (++entries > kExpressionEntries) {
          result.truncated = true;
          break;
        }
        if (!fsys::is_regular_file(item.symlink_status())) continue;
        const auto relative = item.path().lexically_relative(path);
        if (!add(path, relative, relative.string())) break;
      }
      std::ranges::sort(result.files, {}, &ExpressionFile::path);
    }
  });
  return result;
}

auto NixEvaluator::make_available(const NixCall& call,
                                  const DerivationClosureTransport& transport,
                                  const WorkerDerivation& derivation) -> void {
  CallScope scope(call);
  translated(call, [&] { transport.send(derivation); });
}

auto NixEvaluator::realise_native(const NixCall& call, NixResult result)
    -> NixResult {
  return translated(call, [&] {
    return EvaluationState::realise_native(call, std::move(result));
  });
}

}  // namespace tsugumi
