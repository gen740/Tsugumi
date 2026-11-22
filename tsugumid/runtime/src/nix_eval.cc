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
#include <nix/fetchers/filtering-source-accessor.hh>
#include <nix/flake/flake.hh>
#include <nix/flake/flakeref.hh>
#include <nix/flake/settings.hh>
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
#include "tsugumi/protocol/nix_evaluator_generated.h"
#include "tsugumi/util/process.hh"
#include "tsugumi/workerd/log_limits.hh"
#include "tsugumi/workerd/store.hh"

extern char** environ;

namespace tsugumi {

namespace {

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
      : wake_(call, [thread = pthread_self(), build_store] {
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
  if (argc != 5) return 2;

  try {
    NixStore::init_process();
    if (*argv[3])
      nix::settings.getWorkerSettings().maxBuildJobs.set(argv[3], false);
    if (*argv[4])
      nix::settings.getLocalSettings().buildCores.set(argv[4], false);
    // A JSON string retains embedded newlines in a single log entry. The
    // parent can discard an oversized frame without trying to decode it.
    nix::logger = std::make_unique<NixLogSink>(
        [truncated = false](const std::string& line) mutable {
          if (truncated) return;
          if (line.size() >= kInvocationLogLimitBytes) {
            nix::writeFull(STDERR_FILENO, "null\n");
            truncated = true;
            return;
          }
          auto frame = nlohmann::json(line).dump();
          frame += '\n';
          nix::writeFull(STDERR_FILENO, frame);
        });
    auto store = nix::openStore();
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
      nix::writeFull(STDOUT_FILENO, diagnostic);
    return 1;
  }
}

namespace {

struct EvaluationState {
  std::string entry_text;
  std::vector<std::string> allowed;
  // Settings must outlive the EvalState.
  bool read_only = false;
  std::unique_ptr<nix::EvalSettings> eval_settings;
  std::unique_ptr<nix::fetchers::Settings> fetch_settings;
  // Opted-in flakes only: Cards never reach this state, or getFlake.
  std::unique_ptr<nix::EvalSettings> flake_eval_settings;
  std::unique_ptr<nix::flake::Settings> flake_settings;
  std::shared_ptr<nix::EvalState> flake_state;
  std::map<std::string, FlakeInfo>
      flakes;  // by the ref asked for and the locked one
  std::shared_ptr<nix::Store> store;
  std::shared_ptr<nix::EvalState> state;
  std::shared_ptr<EvaluationReads> reads;
  uint64_t epoch = 1;
  std::optional<int64_t> current_time;
  std::optional<nix::PrimOp> path_exists;
  std::map<std::string, std::pair<nix::RootValue, bool>> existence_checks;
  std::vector<std::unique_ptr<nix::PrimOp>> wrapped_ops;
  nix::RootValue entry;
  // The entry's readSource: a declared nixSources flake, evaluated in the
  // flake state for the system of the evaluation in progress.
  nix::RootValue source_reader;
  std::string evaluation_system;
  std::map<std::string, nix::RootValue> pkgs;  // per nixpkgs pin: read once

  auto install(nix::Value& value, nix::PrimOp operation) -> void {
    wrapped_ops.push_back(std::make_unique<nix::PrimOp>(std::move(operation)));
    value.mkPrimOp(wrapped_ops.back().get());
  }

  void open() {
    if (!store) store = nix::openStore().get_ptr();
    eval_settings = std::make_unique<nix::EvalSettings>(read_only);
    nix::loadConfFile(*eval_settings);
    eval_settings->restrictEval = true;
    eval_settings->enableImportFromDerivation = false;
    eval_settings->allowedUris =
        nix::Strings{"https://github.com/NixOS/nixpkgs/archive/"};
    eval_settings->nixPath = nix::Strings{};
    if (!fetch_settings) {
      fetch_settings = std::make_unique<nix::fetchers::Settings>();
      nix::loadConfFile(*fetch_settings);
    }
    state = std::make_shared<nix::EvalState>(nix::LookupPath{},
                                             nix::ref<nix::Store>(store),
                                             *fetch_settings, *eval_settings);
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
    // Preserve the original evaluator constant when filesystem changes require
    // a fresh import/source-to-store cache. getEnv is masked by restrictEval.
    auto& time = state->getBuiltin("currentTime");
    if (!eval_settings->pureEval) {
      if (!current_time)
        current_time =
            state->forceInt(time, nix::noPos, "while reading evaluator time")
                .value;
      time.mkInt(*current_time);
    }
    // Fetchers bypass SourceAccessor. Only hashed fetchurl/fetchTarball are
    // covered; other fetches continue to work but make this state ineligible.
    for (const char* name :
         {"fetchTarball", "fetchurl", "fetchGit", "fetchTree", "fetchMercurial",
          "fetchClosure", "getFlake", "exec", "importNative"}) {
      auto* attr =
          state->getBuiltins().attrs()->get(state->symbols.create(name));
      if (!attr || attr->value->type() != nix::nFunction ||
          !attr->value->isPrimOp())
        continue;
      auto original = *attr->value->primOp();
      auto wrapped = original;
      wrapped.impl = [this, original,
                      hashed = std::string(name) == "fetchTarball" ||
                               std::string(name) == "fetchurl"](
                         nix::EvalState& st, nix::PosIdx pos, nix::Value** args,
                         nix::Value& out) {
        bool covered = false;
        if (hashed) {
          st.forceValue(*args[0], pos);
          if (args[0]->type() == nix::nAttrs) {
            if (auto* hash = args[0]->attrs()->get(st.symbols.create("sha256")))
              covered = !st.forceStringNoCtx(*hash->value, pos,
                                             "while checking the fetch hash")
                             .empty();
          }
        }
        if (!covered) reads->reusable = false;
        original.impl(st, pos, args, out);
      };
      install(*attr->value, std::move(wrapped));
    }
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
    auto read = [this](nix::EvalState& st, nix::PosIdx pos, nix::Value** args,
                       nix::Value& out) {
      st.forceAttrs(*args[0], pos, "while reading a nixSource");
      auto field = [&](const char* name) {
        const auto* attr = args[0]->attrs()->get(st.symbols.create(name));
        if (!attr) throw nix::Error("a nixSource lacks %s", name);
        return std::string(st.forceStringNoCtx(*attr->value, pos,
                                               "while reading a nixSource"));
      };
      const auto name = field("name");
      NixCall call;
      call.system = evaluation_system;
      call.source_overrides = field("overrides");
      FlakeInfo info;
      try {
        info = flake(call, field("ref"));
      } catch (const nix::Interrupted&) {
        throw;
      } catch (const nix::BaseError& e) {
        throw nix::Error("nixSources %s for system %s: %s", name, call.system,
                         message(e));
      } catch (const std::exception& e) {
        throw nix::Error("nixSources %s for system %s: %s", name, call.system,
                         e.what());
      }
      nix::parseJSON(st, info.json, out);
    };
    auto* reader = state->allocValue();
    install(*reader, nix::PrimOp{.name = "readSource",
                                 .args = {"source"},
                                 .arity = 1,
                                 .doc = std::nullopt,
                                 .addTrace = true,
                                 .impl = std::move(read),
                                 .experimentalFeature = std::nullopt,
                                 .internal = false});
    source_reader = nix::allocRootValue(reader);
    auto* v = state->allocValue();
    state->eval(
        state->parseExprFromString(
            entry_text, state->rootPath(nix::CanonPath(allowed.back()))),
        *v);
    entry = nix::allocRootValue(v);
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

  auto refresh() -> void {
    if (!state || unchanged()) return;
    // Untracked fetches cannot be proven unchanged, so they also get a fresh
    // state on the next evaluation rather than a stale cached import.
    existence_checks.clear();
    pkgs.clear();
    entry.reset();
    source_reader.reset();
    state.reset();
    wrapped_ops.clear();
    ++epoch;
    open();
  }

  nix::Value& pkgs_for(const NixCall& c) {
    std::string key = c.pin_url + "#" + c.pin_hash + "#" + c.system;
    if (auto it = pkgs.find(key); it != pkgs.end()) return **it->second;
    auto quote = [](const std::string& s) {
      std::string q = "\"";
      for (char ch : s) {
        if (ch == '"' || ch == '\\' || ch == '$') q += '\\';
        q += ch;
      }
      return q + "\"";
    };
    std::string expr =
        "import (builtins.fetchTarball { url = " + quote(c.pin_url) +
        "; sha256 = " + quote(c.pin_hash) +
        "; }) { system = " + quote(c.system) +
        "; config = { }; overlays = [ ]; }";
    auto* v = state->allocValue();
    state->eval(
        state->parseExprFromString(expr, state->rootPath(nix::CanonPath("/"))),
        *v);
    state->forceValue(*v, nix::noPos);
    return **(pkgs[key] = nix::allocRootValue(v));
  }

  void open_flakes() {
    if (!state) open();
    nix::experimentalFeatureSettings.set("extra-experimental-features",
                                         "flakes");
    if (!flake_settings)
      flake_settings = std::make_unique<nix::flake::Settings>();
    flake_eval_settings = std::make_unique<nix::EvalSettings>(read_only);
    nix::loadConfFile(*flake_eval_settings);
    flake_eval_settings->pureEval = true;
    flake_eval_settings->enableImportFromDerivation = false;
    flake_eval_settings->nixPath = nix::Strings{};
    flake_settings->configureEvalSettings(*flake_eval_settings);
    flake_state = std::make_shared<nix::EvalState>(
        nix::LookupPath{}, nix::ref<nix::Store>(store), *fetch_settings,
        *flake_eval_settings);
  }

  // Every .drv of a remembered result is still there, and stays while this
  // process does.
  bool hold(const FlakeInfo& info) {
    for (const auto& [attr, p] :
         nlohmann::json::parse(info.json).at("packages").items()) {
      auto drv = store->parseStorePath(p.at("drv").get<std::string>());
      if (!store->isValidPath(drv)) return false;
      store->addTempRoot(drv);
    }
    return true;
  }

  FlakeInfo flake(const NixCall& c, const std::string& ref) {
    if (!flake_state) open_flakes();
    std::string system =
        c.system.empty() ? flake_eval_settings->getCurrentSystem() : c.system;
    const auto customization = "|" + c.source_overrides;
    if (auto it = flakes.find(ref + "|" + system + customization);
        it != flakes.end()) {
      if (hold(it->second)) return it->second;
      // Collected since: this state remembers derivations it would not write
      // again.
      flakes.clear();
      open_flakes();
    }
    auto quote = [](const std::string& s) {
      std::string q = "\"";
      for (char ch : s) {
        if (ch == '"' || ch == '\\' || ch == '$') q += '\\';
        q += ch;
      }
      return q + "\"";
    };
    auto input = nix::parseFlakeRef(*fetch_settings, ref);
    auto locked = nix::flake::getFlake(*flake_state, input,
                                       nix::fetchers::UseRegistries::No)
                      .lockedRef.to_string();
    std::string expr = R"(
      let
        f = builtins.getFlake )" +
                       quote(locked) + R"(;
        custom = builtins.fromJSON )" +
                       quote(c.source_overrides) + R"(;
        # JSON values only: no caller-provided expression runs in the source evaluator.
        convert = v:
          if builtins.isList v then map convert v
          else if builtins.isAttrs v then
            if builtins.attrNames v == [ "fetchurl" ] then
              let spec = v.fetchurl; in
              if !(spec ? url && spec ? sha256) then
                throw "nixSources overrides: fetchurl needs url and sha256"
              else builtins.fetchurl { inherit (spec) url sha256; }
            else builtins.mapAttrs (_: convert) v
          else v;
        customize = name: package:
          let
            config = custom.${name} or {};
            args = convert (config.override or {});
            attrs = convert (config.overrideAttrs or {});
            overridden = if !(config ? override) then package
              else if package ? override then package.override args
              else throw "nixSources overrides: package ${name} does not support override; use overrideAttrs when available";
          in if !(config ? overrideAttrs) then overridden
             else if overridden ? overrideAttrs then overridden.overrideAttrs (_: attrs)
             else throw "nixSources overrides: package ${name} does not support overrideAttrs";
        plain = builtins.unsafeDiscardStringContext;
        read = d: builtins.tryEval (builtins.deepSeq [ d.drvPath d.outPath ] {
          name = d.name or "";
          drv = plain d.drvPath;  # not outPath: Nix would write the whole set as that path
          out = plain d.outPath;
          output = d.outputName or "out";
        });
        packages = f.packages.)" +
                       quote(system) + R"( or { };
        missing = builtins.filter (name: !(builtins.hasAttr name packages)) (builtins.attrNames custom);
        all = if missing != [] then throw "nixSources overrides: unknown package ${builtins.head missing}"
          else builtins.mapAttrs (name: package:
            if builtins.hasAttr name custom then
              let d = customize name package;
              in { success = true; value = { name = d.name or ""; drv = plain d.drvPath;
                    out = plain d.outPath; output = d.outputName or "out"; }; }
            else read package) packages;
      in {
        ref = )" + quote(locked) +
                       R"(;
        rev = f.rev or "";
        narHash = f.narHash or "";
        packages = builtins.listToAttrs (builtins.concatMap
          (n: if all.${n}.success then [ { name = n; value = all.${n}.value; } ] else [ ])
          (builtins.attrNames all));
      }
    )";
    auto* v = flake_state->allocValue();
    flake_state->eval(flake_state->parseExprFromString(
                          expr, flake_state->rootPath(nix::CanonPath("/"))),
                      *v);
    nix::NixStringContext context;
    auto json = nix::printValueAsJSON(*flake_state, true, *v, nix::noPos,
                                      context, false);
    FlakeInfo info;
    info.ref = locked;
    info.rev = json.at("rev").get<std::string>();
    info.nar_hash = json.at("narHash").get<std::string>();
    for (const auto& [attr, p] : json.at("packages").items())
      info.packages.push_back(attr);
    info.json = json.dump();
    hold(info);
    flakes[ref + "|" + system + customization] = info;
    flakes[locked + "|" + system + customization] = info;
    return info;
  }

  auto evaluate(const NixCall& c) -> NixResult {
    refresh();
    if (!state) open();
    auto& vpkgs = pkgs_for(c);
    auto* vargs = state->allocValue();
    state->eval(state->parseExprFromString(
                    c.args, state->rootPath(nix::CanonPath("/"))),
                *vargs);
    evaluation_system = c.system;
    auto* with_pkgs = state->allocValue();
    state->callFunction(**entry, vpkgs, *with_pkgs, nix::noPos);
    auto* partial = state->allocValue();
    state->callFunction(*with_pkgs, **source_reader, *partial, nix::noPos);
    auto* vres = state->allocValue();
    state->callFunction(*partial, *vargs, *vres, nix::noPos);
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
    auto& plan = attr("plan");
    state->forceValue(plan, nix::noPos);
    auto drv = nix::getDerivation(*state, plan, false);
    if (!drv)
      throw std::runtime_error("the Card build plan is not a derivation");
    auto out = drv->queryOutPath();
    auto drv_path = drv->requireDrvPath();
    r.derivation = {store->printStorePath(drv_path), store->printStorePath(out),
                    c.system};
    if (store->readDerivation(drv_path).platform != c.system)
      throw std::runtime_error("Card derivation does not target system " +
                               c.system);
    store->addTempRoot(drv_path);
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

  // The Nix client sends these options with its own daemon connection. The
  // evaluator and the other builds can keep using their own settings.
  static auto build(const NixCall& c, const std::string& drv) -> void {
    const auto executable = nix::getSelfExe();
    if (!executable)
      throw std::runtime_error("cannot locate the Nix build client");
    proc::Spec spec;
    spec.argv = {executable->string(), "--nix-build-client", drv,
                 c.jobs ? std::to_string(*c.jobs) : "",
                 c.cores ? std::to_string(*c.cores) : ""};
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
          for (const auto& line : chunk.lines) {
            if (line == "null\n") {
              chunk.truncated = true;
              break;
            }
            if (line.starts_with('"')) {
              auto entry = nlohmann::json::parse(line, nullptr, false);
              if (auto* text = entry.get_ptr<std::string*>()) {
                output.push_back(std::move(*text));
                continue;
              }
            }
            // Keep diagnostics printed before the child installs its logger.
            output.push_back(tsugumi::text::replace_invalid_utf8(line));
          }
          c.log({output, chunk.truncated});
        },
        kInvocationLogLimitBytes);
    if (result.cancelled) throw NixInterrupted(false, "Nix cancelled");
    if (result.timed_out)
      throw NixInterrupted(true, "Nix exceeded buildTimeoutSeconds");
    if (!proc::exited_ok(result.status))
      throw std::runtime_error(result.out.empty()
                                   ? "Nix build failed: " +
                                         proc::describe(result.status)
                                   : result.out);
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
      refresh();
      return false;
    }
    auto drv = store->parseStorePath(result.derivation.path);
    auto out = store->parseStorePath(result.derivation.output);
    store->addTempRoot(drv);
    store->addTempRoot(out);
    if (store->isValidPath(drv) && store->isValidPath(out)) return true;
    // Nix remembers derivations it may not write a second time after GC.
    existence_checks.clear();
    pkgs.clear();
    entry.reset();
    source_reader.reset();
    state.reset();
    wrapped_ops.clear();
    ++epoch;
    return false;
  }
};

namespace E = Tsugumi::NixEvaluator;

template <class Table>
auto receive(nix::Source& source) -> typename Table::NativeTableType {
  const auto bytes = nix::readString(source);
  flatbuffers::Verifier verifier(
      reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
  if (!verifier.VerifyBuffer<Table>(nullptr))
    throw std::runtime_error("invalid Nix evaluator message");
  typename Table::NativeTableType result;
  flatbuffers::GetRoot<Table>(bytes.data())->UnPackTo(&result);
  return result;
}

template <class Table, class Message>
auto send(nix::FdSink& sink, Message&& message, std::string system = {}) -> void {
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
  EvaluationState eval;
  eval.entry_text = settings->entry;
  eval.allowed = settings->allowed;
  eval.epoch = settings->epoch;
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
      call.pin_url = op->pin_url;
      call.pin_hash = op->pin_hash;
      call.args = op->args;
      call.max_implementations = op->max_implementations;
      const auto result = eval.evaluate(call);
      E::EvaluationT reply;
      reply.drv = result.derivation.path;
      reply.out = result.derivation.output;
      reply.execution_list = result.execution_list;
      reply.implementation_count = result.implementation_count;
      reply.implementation_labels = result.implementation_labels;
      reply.epoch = result.evaluation_epoch;
      send<E::Reply>(sink, std::move(reply));
    } else if (const auto* op = request.body.AsFlake()) {
      call.source_overrides = op->overrides;
      auto result = eval.flake(call, op->ref);
      E::FlakeInfoT reply;
      reply.ref = std::move(result.ref);
      reply.rev = std::move(result.rev);
      reply.nar_hash = std::move(result.nar_hash);
      reply.packages = std::move(result.packages);
      reply.nix_json = std::move(result.json);
      send<E::Reply>(sink, std::move(reply));
    } else if (const auto* op = request.body.AsReuse()) {
      NixResult result;
      result.evaluation_epoch = op->epoch;
      result.derivation.path = op->drv;
      result.derivation.output = op->out;
      E::ReusableT reply;
      reply.value = eval.reuse(result);
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
  std::vector<std::string> allowed;
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
    for (auto e = environ; *e; ++e) spec.env.emplace_back(*e);
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
        settings.allowed = allowed;
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

NixEvaluator::NixEvaluator(std::string entry, std::vector<std::string> allowed)
    : impl_(std::make_unique<Impl>()) {
  impl_->entry = std::move(entry);
  impl_->allowed = std::move(allowed);
}

NixEvaluator::~NixEvaluator() = default;

auto NixEvaluator::evaluate(const NixCall& call) -> NixResult {
  if (call.system.empty())
    throw std::runtime_error("execution target system is unknown");
  E::EvaluateT request;
  request.pin_url = call.pin_url;
  request.pin_hash = call.pin_hash;
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

auto NixEvaluator::flake(const NixCall& call, const std::string& ref)
    -> FlakeInfo {
  E::FlakeT request;
  request.ref = ref;
  request.overrides = call.source_overrides;
  return impl_->exchange(call, std::move(request), [&](auto& reply) {
    auto* data = reply.AsFlakeInfo();
    if (!data) throw std::runtime_error("missing Nix flake reply");
    FlakeInfo info{std::move(data->ref), std::move(data->rev),
                   std::move(data->nar_hash), std::move(data->packages),
                   std::move(data->nix_json)};
    for (const auto& [attr, package] :
         nlohmann::json::parse(info.json).at("packages").items())
      impl_->hold(package.at("drv").get<std::string>());
    return info;
  });
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
