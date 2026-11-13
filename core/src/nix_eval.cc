// Built as C++23 (the Nix headers require it); the rest of the core is C++20
// and never includes a Nix header.
#include "nix_eval.hh"

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <condition_variable>
#include <deque>
#include <fstream>
#include <future>
#include <mutex>
#include <sstream>
#include <thread>

#include <nix/expr/eval-gc.hh>
#include <nix/expr/eval-settings.hh>
#include <nix/expr/eval.hh>
#include <nix/expr/get-drvs.hh>
#include <nix/expr/value-to-json.hh>
#include <nix/fetchers/fetch-settings.hh>
#include <nix/flake/flake.hh>
#include <nix/flake/flakeref.hh>
#include <nix/flake/settings.hh>
#include <nix/store/derived-path.hh>
#include <nix/store/derivations.hh>
#include <nix/store/globals.hh>
#include <nix/store/indirect-root-store.hh>
#include <nix/store/store-api.hh>
#include <nix/store/store-open.hh>
#include <nix/util/logging.hh>
#include <nix/util/serialise.hh>
#include <nix/util/signals.hh>
#include <nix/util/terminal.hh>

namespace tsugumi {

namespace {

// Where Nix's messages go: the sink of the call in progress (calls run one
// at a time on the evaluator's thread).
class SinkLogger : public nix::Logger {
 public:
  void set(std::function<void(const std::string&)> f) {
    std::lock_guard<std::mutex> l(mu_);
    sink_ = std::move(f);
  }
  void log(nix::Verbosity lvl, std::string_view s) override {
    if (lvl <= nix::lvlInfo) emit(std::string(s));
  }
  void logEI(const nix::ErrorInfo& ei) override {
    std::ostringstream os;
    nix::showErrorInfo(os, ei, false);
    emit(os.str());
  }
  void startActivity(nix::ActivityId, nix::Verbosity lvl, nix::ActivityType, const std::string& s, const Fields&,
                     nix::ActivityId) override {
    // "building '/nix/store/...drv'", "copying path ... from 'https://cache.nixos.org'"
    if (lvl <= nix::lvlInfo && !s.empty()) emit(s);
  }
  void result(nix::ActivityId, nix::ResultType type, const Fields& fields) override {
    if ((type == nix::resBuildLogLine || type == nix::resPostBuildLogLine) && !fields.empty() &&
        fields[0].type == Field::tString)
      emit(fields[0].s);
  }

 private:
  void emit(const std::string& line) {
    std::lock_guard<std::mutex> l(mu_);
    if (sink_) sink_(nix::filterANSIEscapes(line, true) + "\n");
  }
  std::mutex mu_;
  std::function<void(const std::string&)> sink_;
};

SinkLogger* g_logger = nullptr;

void on_usr1(int) {}  // interrupts a blocking read of the daemon connection

std::string message(const nix::BaseError& e) { return nix::filterANSIEscapes(e.what(), true); }

}  // namespace

void NixEvaluator::init_process() {
  static bool done = false;
  if (done) return;
  done = true;
  nix::initLibStore(true);  // nix.conf: substituters, the daemon socket, ...
  nix::initGC();
  auto logger = std::make_unique<SinkLogger>();
  g_logger = logger.get();
  nix::logger = std::move(logger);
  struct sigaction sa {};
  sa.sa_handler = on_usr1;  // no SA_RESTART: the read returns EINTR and Nix checks for interrupts
  sigemptyset(&sa.sa_mask);
  ::sigaction(SIGUSR1, &sa, nullptr);
}

std::vector<std::string> NixEvaluator::substituters() {
  std::vector<std::string> urls;
  for (const auto& ref : nix::settings.getWorkerSettings().substituters.get()) urls.push_back(ref.render(false));
  return urls;
}

struct NixEvaluator::Impl {
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
  std::map<std::string, FlakeInfo> flakes;  // by the ref asked for and the locked one
  std::shared_ptr<nix::Store> store;
  std::shared_ptr<nix::EvalState> state;
  nix::RootValue entry;
  std::map<std::string, nix::RootValue> pkgs;  // per nixpkgs pin: read once

  pthread_t thread{};
  std::mutex mu;
  std::condition_variable cv;
  std::deque<std::function<void()>> tasks;
  bool stopping = false;

  // The call in progress, for the interrupt check (very frequent: no lock)
  // and the watchdog.
  std::atomic<const NixCall*> current{nullptr};
  std::thread watchdog;
  std::atomic<bool> stop_watchdog{false};

  bool interrupted(const NixCall& c, bool* timed_out) {
    if (c.cancel && c.cancel->load()) return true;
    if (c.deadline && std::chrono::steady_clock::now() >= *c.deadline) {
      if (timed_out) *timed_out = true;
      return true;
    }
    return false;
  }

  static void* main(void* self) {
    auto* d = static_cast<Impl*>(self);
    // Registered with Boehm GC by GC_pthread_create (gc.h redirects
    // pthread_create in this file), so it is scanned while it evaluates.
    nix::unix::interruptCheck = [d] {
      const NixCall* c = d->current.load(std::memory_order_relaxed);
      return c && d->interrupted(*c, nullptr);
    };
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> l(d->mu);
        d->cv.wait(l, [&] { return d->stopping || !d->tasks.empty(); });
        if (d->tasks.empty()) break;
        task = std::move(d->tasks.front());
        d->tasks.pop_front();
      }
      task();
    }
    d->state.reset();
    d->pkgs.clear();
    d->entry.reset();
    return nullptr;
  }

  // Runs `f` on the evaluator's thread and waits for it.
  template <class F>
  auto run(F&& f) -> decltype(f()) {
    std::packaged_task<decltype(f())()> task(std::forward<F>(f));
    auto fut = task.get_future();
    {
      std::lock_guard<std::mutex> l(mu);
      tasks.emplace_back([&task] { task(); });
    }
    cv.notify_all();
    return fut.get();
  }

  // Runs on the evaluator's thread; Nix's errors become what NixEvaluator promises.
  template <class F>
  auto guarded(const NixCall& call, F f) -> decltype(f()) {
    return run([&]() -> decltype(f()) {
      try {
        return f();
      } catch (const nix::Interrupted&) {
        bool timed_out = false;
        interrupted(call, &timed_out);
        throw NixInterrupted(timed_out, timed_out ? "Nix exceeded buildTimeoutSeconds" : "Nix cancelled");
      } catch (const nix::BaseError& e) {
        if (interrupted(call, nullptr)) {
          bool timed_out = false;
          interrupted(call, &timed_out);
          throw NixInterrupted(timed_out, timed_out ? "Nix exceeded buildTimeoutSeconds" : "Nix cancelled");
        }
        throw std::runtime_error(message(e));
      }
    });
  }

  void open() {
    store = nix::openStore().get_ptr();
    eval_settings = std::make_unique<nix::EvalSettings>(read_only);
    nix::loadConfFile(*eval_settings);
    eval_settings->restrictEval = true;
    eval_settings->enableImportFromDerivation = false;
    eval_settings->allowedUris = nix::Strings{"https://github.com/NixOS/nixpkgs/archive/"};
    eval_settings->nixPath = nix::Strings{};
    fetch_settings = std::make_unique<nix::fetchers::Settings>();
    nix::loadConfFile(*fetch_settings);
    state = std::make_shared<nix::EvalState>(nix::LookupPath{}, nix::ref<nix::Store>(store), *fetch_settings,
                                             *eval_settings);
    for (const auto& p : allowed) state->allowPathLegacy(p);
    auto* v = state->allocValue();
    state->eval(state->parseExprFromString(entry_text, state->rootPath(nix::CanonPath(allowed.back()))), *v);
    entry = nix::allocRootValue(v);
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
    std::string expr = "import (builtins.fetchTarball { url = " + quote(c.pin_url) + "; sha256 = " +
                       quote(c.pin_hash) + "; }) { system = " +
                       quote(c.system) +
                       "; config = { }; overlays = [ ]; }";
    auto* v = state->allocValue();
    state->eval(state->parseExprFromString(expr, state->rootPath(nix::CanonPath("/"))), *v);
    state->forceValue(*v, nix::noPos);
    return **(pkgs[key] = nix::allocRootValue(v));
  }

  void open_flakes() {
    if (!state) open();
    nix::experimentalFeatureSettings.set("extra-experimental-features", "flakes");
    if (!flake_settings) flake_settings = std::make_unique<nix::flake::Settings>();
    flake_eval_settings = std::make_unique<nix::EvalSettings>(read_only);
    nix::loadConfFile(*flake_eval_settings);
    flake_eval_settings->pureEval = true;
    flake_eval_settings->enableImportFromDerivation = false;
    flake_eval_settings->nixPath = nix::Strings{};
    flake_settings->configureEvalSettings(*flake_eval_settings);
    flake_state = std::make_shared<nix::EvalState>(nix::LookupPath{}, nix::ref<nix::Store>(store), *fetch_settings,
                                                   *flake_eval_settings);
  }

  // Every .drv of a remembered result is still there, and stays while this process does.
  bool hold(const FlakeInfo& info) {
    for (const auto& [attr, p] : nlohmann::json::parse(info.json).at("packages").items()) {
      auto drv = store->parseStorePath(p.at("drv").get<std::string>());
      if (!store->isValidPath(drv)) return false;
      store->addTempRoot(drv);
    }
    return true;
  }

  FlakeInfo flake(const NixCall& c, const std::string& ref) {
    current = &c;
    g_logger->set(c.log);
    struct Done {
      Impl* d;
      ~Done() {
        g_logger->set(nullptr);
        d->current = nullptr;
      }
    } done{this};
    if (!flake_state) open_flakes();
    std::string system = c.system.empty() ? flake_eval_settings->getCurrentSystem() : c.system;
    const auto customization = "|" + c.source_overrides;
    if (auto it = flakes.find(ref + "|" + system + customization); it != flakes.end()) {
      if (hold(it->second)) return it->second;
      // Collected since: this state remembers derivations it would not write again.
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
    auto locked = nix::flake::getFlake(*flake_state, input, nix::fetchers::UseRegistries::No).lockedRef.to_string();
    std::string expr = R"(
      let
        f = builtins.getFlake )" + quote(locked) + R"(;
        custom = builtins.fromJSON )" + quote(c.source_overrides) + R"(;
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
        packages = f.packages.)" + quote(system) + R"( or { };
        missing = builtins.filter (name: !(builtins.hasAttr name packages)) (builtins.attrNames custom);
        all = if missing != [] then throw "nixSources overrides: unknown package ${builtins.head missing}"
          else builtins.mapAttrs (name: package:
            if builtins.hasAttr name custom then
              let d = customize name package;
              in { success = true; value = { name = d.name or ""; drv = plain d.drvPath;
                    out = plain d.outPath; output = d.outputName or "out"; }; }
            else read package) packages;
      in {
        ref = )" + quote(locked) + R"(;
        rev = f.rev or "";
        narHash = f.narHash or "";
        packages = builtins.listToAttrs (builtins.concatMap
          (n: if all.${n}.success then [ { name = n; value = all.${n}.value; } ] else [ ])
          (builtins.attrNames all));
      }
    )";
    auto* v = flake_state->allocValue();
    flake_state->eval(flake_state->parseExprFromString(expr, flake_state->rootPath(nix::CanonPath("/"))), *v);
    nix::NixStringContext context;
    auto json = nix::printValueAsJSON(*flake_state, true, *v, nix::noPos, context, false);
    FlakeInfo info;
    info.ref = locked;
    info.rev = json.at("rev").get<std::string>();
    info.nar_hash = json.at("narHash").get<std::string>();
    for (const auto& [attr, p] : json.at("packages").items()) info.packages.push_back(attr);
    info.json = json.dump();
    hold(info);
    flakes[ref + "|" + system + customization] = info;
    flakes[locked + "|" + system + customization] = info;
    return info;
  }

  auto evaluate(const NixCall& c) -> NixResult {
    current = &c;
    g_logger->set(c.log);
    struct Done {
      Impl* d;
      ~Done() {
        g_logger->set(nullptr);
        d->current = nullptr;
      }
    } done{this};
    if (!state) open();
    if (c.on_phase) c.on_phase(false);
    auto& vpkgs = pkgs_for(c);
    auto* vargs = state->allocValue();
    state->eval(state->parseExprFromString(c.args, state->rootPath(nix::CanonPath("/"))), *vargs);
    auto* partial = state->allocValue();
    state->callFunction(**entry, vpkgs, *partial, nix::noPos);
    auto* vres = state->allocValue();
    state->callFunction(*partial, *vargs, *vres, nix::noPos);
    state->forceAttrs(*vres, nix::noPos, "while evaluating the Card execution plan");
    auto attr = [&](const char* name) -> nix::Value& {
      return *state->getAttr(state->symbols.create(name), vres->attrs(), "while reading the Card execution plan")->value;
    };
    NixResult r;
    r.execution_list = state->forceBool(attr("executionList"), nix::noPos, "while reading the Card run shape");
    const auto count = state->forceInt(attr("implementationCount"), nix::noPos, "while counting Card implementations").value;
    if (count < 0 || count > c.max_implementations)
      throw NixImplementationLimit("input and run list expansion exceeds 256 combinations");
    r.implementation_count = static_cast<uint32_t>(count);
    auto& manifest = attr("manifest");
    state->forceValue(manifest, nix::noPos);
    auto drv = nix::getDerivation(*state, manifest, false);
    if (!drv) throw std::runtime_error("the Card manifest is not a derivation");
    auto out = drv->queryOutPath();
    auto drv_path = drv->requireDrvPath();
    r.derivation = {store->printStorePath(drv_path), store->printStorePath(out), c.system};
    if (store->readDerivation(drv_path).platform != c.system)
      throw std::runtime_error("Card derivation does not target system " + c.system);
    store->addTempRoot(drv_path);
    return r;
  }

  auto realise_native(const NixCall& c, NixResult r) -> NixResult {
    const auto drv_path = store->parseStorePath(r.derivation.path);
    const auto out = store->parseStorePath(r.derivation.output);
    // Held by this process's daemon connection from here: a concurrent GC
    // never removes it between the check, the build and the permanent root.
    store->addTempRoot(out);
    if (!store->isValidPath(out)) {
      if (c.on_phase) c.on_phase(true);
      store->buildPaths({nix::DerivedPath::Built{.drvPath = nix::makeConstantStorePathRef(drv_path),
                                                 .outputs = nix::OutputsSpec::All{}}});
      r.built = true;
    }
    nix::StorePathSet closure;
    store->computeFSClosure(out, closure);
    for (const auto& path : closure) r.closure.push_back(store->printStorePath(path));
    std::sort(r.closure.begin(), r.closure.end());
    if (!c.root_dir.empty()) {
      r.root = c.root_dir / (c.root_prefix + std::string(out.to_string()));
      add_root(r.derivation.output, r.root);
    }
    std::ifstream f(r.derivation.output, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    r.manifest = ss.str();
    return r;
  }

  // The project's permanent root: a symlink, recreated on every use (its age
  // tells when the result was last needed).
  void add_root(const std::string& out, const std::filesystem::path& link) {
    std::filesystem::create_directories(link.parent_path());
    auto tmp = link;
    tmp += ".tmp-" + std::to_string(::getpid());
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    std::filesystem::create_symlink(out, tmp);
    std::filesystem::rename(tmp, link);
    std::string l = link.string();
    if (!l.starts_with("/nix/var/nix/gcroots/")) {
      // Outside the GC root folder (a project on a development host): the
      // daemon records it as an indirect root.
      if (auto* ir = dynamic_cast<nix::IndirectRootStore*>(store.get())) ir->addIndirectRoot(link);
    }
  }
};

FlakeInfo NixEvaluator::flake(const NixCall& call, const std::string& ref) {
  return impl_->guarded(call, [&] { return impl_->flake(call, ref); });
}

NixEvaluator::NixEvaluator(std::string entry, std::vector<std::string> allowed) : impl_(std::make_unique<Impl>()) {
  impl_->entry_text = std::move(entry);
  impl_->allowed = std::move(allowed);
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 64u << 20);  // deep evaluations (nixpkgs); max-call-depth bounds them
  if (pthread_create(&impl_->thread, &attr, &Impl::main, impl_.get()) != 0)
    throw std::runtime_error("cannot start the Nix evaluator thread");
  pthread_attr_destroy(&attr);
  // While a call is interrupted, a blocked read of the daemon connection is
  // woken until the call notices.
  impl_->watchdog = std::thread([d = impl_.get()] {
    while (!d->stop_watchdog) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      const NixCall* c = d->current.load();
      if (c && d->interrupted(*c, nullptr)) pthread_kill(d->thread, SIGUSR1);
    }
  });
}

NixEvaluator::~NixEvaluator() {
  impl_->stop_watchdog = true;
  if (impl_->watchdog.joinable()) impl_->watchdog.join();
  {
    std::lock_guard<std::mutex> l(impl_->mu);
    impl_->stopping = true;
  }
  impl_->cv.notify_all();
  pthread_join(impl_->thread, nullptr);
}

auto NixEvaluator::evaluate(const NixCall& call) -> NixResult {
  if (call.system.empty()) throw std::runtime_error("execution target system is unknown");
  return impl_->guarded(call, [&] { return impl_->evaluate(call); });
}

auto NixEvaluator::make_available(const NixCall& call, const DerivationClosureTransport& transport,
                                  const WorkerDerivation& derivation) -> void {
  impl_->guarded(call, [&] {
    impl_->current = &call;
    g_logger->set(call.log);
    struct Done {
      Impl* impl;
      ~Done() {
        g_logger->set(nullptr);
        impl->current = nullptr;
      }
    } done{impl_.get()};
    transport.send(derivation);
  });
}

auto NixEvaluator::realise_native(const NixCall& call, NixResult result) -> NixResult {
  return impl_->guarded(call, [&] {
    impl_->current = &call;
    g_logger->set(call.log);
    struct Done {
      Impl* impl;
      ~Done() {
        g_logger->set(nullptr);
        impl->current = nullptr;
      }
    } done{impl_.get()};
    return impl_->realise_native(call, std::move(result));
  });
}

auto RemoteWorkerStore::realise(const DerivationClosureTransport& transport, const std::filesystem::path& store_root,
                              const WorkerDerivation& derivation,
                              const std::function<void(const std::string&)>& log) -> NixResult {
  g_logger->set(log);
  struct ClearLog {
    ~ClearLog() { g_logger->set(nullptr); }
  } clear_log;
  nix::settings.getLocalSettings().sandboxMode = nix::smEnabled;
  nix::settings.getLocalSettings().sandboxFallback = false;
  auto store = nix::openStore("local", {{"root", store_root.string()}});
  const auto drv = store->parseStorePath(derivation.path);
  const auto out = store->parseStorePath(derivation.output);
  store->addTempRoot(drv);
  store->addTempRoot(out);
  transport.receive(*store, derivation);
  if (store->readDerivation(drv).platform != derivation.system)
    throw std::runtime_error("remote derivation has a different system");
  const auto declared = store->queryDerivationOutputMap(drv);
  const auto declared_out = declared.find("out");
  if (declared_out == declared.end() || declared_out->second != out)
    throw std::runtime_error("remote Nix derivation has a different output path");
  NixResult result;
  if (!store->isValidPath(out)) {
    store->buildPaths({nix::DerivedPath::Built{.drvPath = nix::makeConstantStorePathRef(drv),
                                              .outputs = nix::OutputsSpec::All{}}});
    result.built = true;
  }
  if (!store->isValidPath(out)) throw std::runtime_error("remote Nix build produced no Card manifest");
  nix::StorePathSet closure;
  store->computeFSClosure(out, closure);
  for (const auto& path : closure) result.closure.push_back(store->printStorePath(path));
  std::sort(result.closure.begin(), result.closure.end());
  const auto manifest = store_root / std::filesystem::path(derivation.output).relative_path();
  const int fd = ::open(manifest.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) throw std::runtime_error("cannot open the remote Nix Card manifest");
  nix::AutoCloseFD file{fd};
  struct stat info{};
  if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 || info.st_size > 16 * 1024 * 1024)
    throw std::runtime_error("invalid remote Nix Card manifest size or type");
  result.manifest.resize(static_cast<size_t>(info.st_size));
  size_t offset = 0;
  while (offset < result.manifest.size()) {
    const ssize_t n = ::read(fd, result.manifest.data() + offset, result.manifest.size() - offset);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) throw std::runtime_error("cannot read the remote Nix Card manifest");
    offset += static_cast<size_t>(n);
  }
  result.derivation = derivation;
  return result;
}

bool NixEvaluator::valid(const std::string& store_path) {
  return impl_->run([&]() -> bool {
    try {
      if (!impl_->store) impl_->open();
      return impl_->store->isValidPath(impl_->store->parseStorePath(store_path));
    } catch (const nix::BaseError&) {
      return false;
    }
  });
}

uint64_t NixEvaluator::heap_bytes() { return GC_get_heap_size(); }

auto NixEvaluator::rooted_usage(const std::vector<std::vector<std::string>>& roots)
    -> std::vector<std::map<std::string, uint64_t>> {
  auto store = nix::openStore();
  std::vector<std::map<std::string, uint64_t>> result;
  std::map<std::string, uint64_t> sizes;
  for (const auto& project : roots) {
    nix::StorePathSet closure;
    for (const auto& path : project)
      store->computeFSClosure(store->parseStorePath(path), closure, false,
                              nix::settings.getLocalSettings().keepOutputs,
                              nix::settings.getLocalSettings().keepDerivations);
    auto& measured = result.emplace_back();
    for (const auto& path : closure) {
      auto name = store->printStorePath(path);
      auto [it, inserted] = sizes.try_emplace(name, 0);
      if (inserted) it->second = store->queryPathInfo(path)->narSize;
      measured.emplace(name, it->second);
    }
  }
  return result;
}

}  // namespace tsugumi
