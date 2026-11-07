// Built as C++23 (the Nix headers require it); the rest of the core is C++20
// and never includes a Nix header.
#include "nix_eval.hh"

#include <pthread.h>
#include <signal.h>
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
#include <nix/fetchers/fetch-settings.hh>
#include <nix/store/derived-path.hh>
#include <nix/store/globals.hh>
#include <nix/store/indirect-root-store.hh>
#include <nix/store/store-api.hh>
#include <nix/store/store-open.hh>
#include <nix/util/logging.hh>
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

constexpr const char* kNixpkgsArchives = "https://github.com/NixOS/nixpkgs/archive/";

struct NixEvaluator::Impl {
  std::string entry_text;
  std::vector<std::string> allowed;
  // Settings must outlive the EvalState.
  bool read_only = false;
  std::unique_ptr<nix::EvalSettings> eval_settings;
  std::unique_ptr<nix::fetchers::Settings> fetch_settings;
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

  void open() {
    store = nix::openStore().get_ptr();
    eval_settings = std::make_unique<nix::EvalSettings>(read_only);
    nix::loadConfFile(*eval_settings);
    eval_settings->restrictEval = true;
    eval_settings->enableImportFromDerivation = false;
    eval_settings->allowedUris = nix::Strings{kNixpkgsArchives};
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
    std::string key = c.pin_url + "#" + c.pin_hash;
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
                       quote(c.pin_hash) + "; }) { system = builtins.currentSystem; config = { }; overlays = [ ]; }";
    auto* v = state->allocValue();
    state->eval(state->parseExprFromString(expr, state->rootPath(nix::CanonPath("/"))), *v);
    state->forceValue(*v, nix::noPos);
    return **(pkgs[key] = nix::allocRootValue(v));
  }

  NixResult build(const NixCall& c) {
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
    // One evaluator serves every Card: a Card may fetch what its own project opted in to.
    auto uris = nix::Strings{kNixpkgsArchives};
    uris.insert(uris.end(), c.allowed_uris.begin(), c.allowed_uris.end());
    eval_settings->allowedUris = uris;
    if (c.on_phase) c.on_phase(false);
    auto& vpkgs = pkgs_for(c);
    auto* vargs = state->allocValue();
    state->eval(state->parseExprFromString(c.args, state->rootPath(nix::CanonPath("/"))), *vargs);
    auto* partial = state->allocValue();
    state->callFunction(**entry, vpkgs, *partial, nix::noPos);
    auto* vres = state->allocValue();
    state->callFunction(*partial, *vargs, *vres, nix::noPos);
    state->forceValue(*vres, nix::noPos);
    auto drv = nix::getDerivation(*state, *vres, false);
    if (!drv) throw std::runtime_error("the Card manifest is not a derivation");
    auto out = drv->queryOutPath();
    auto drv_path = drv->requireDrvPath();
    // Held by this process's daemon connection from here: a concurrent GC
    // never removes it between the check, the build and the permanent root.
    store->addTempRoot(out);
    NixResult r;
    if (!store->isValidPath(out)) {
      if (c.on_phase) c.on_phase(true);
      store->buildPaths({nix::DerivedPath::Built{.drvPath = nix::makeConstantStorePathRef(drv_path),
                                                 .outputs = nix::OutputsSpec::All{}}});
      r.built = true;
    }
    r.out = store->printStorePath(out);
    if (!c.root_dir.empty()) {
      r.root = c.root_dir / (c.root_prefix + std::string(out.to_string()));
      add_root(r.out, r.root);
    }
    std::ifstream f(r.out, std::ios::binary);
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

NixResult NixEvaluator::build(const NixCall& call) {
  return impl_->run([&]() -> NixResult {
    try {
      return impl_->build(call);
    } catch (const nix::Interrupted&) {
      bool timed_out = false;
      impl_->interrupted(call, &timed_out);
      throw NixInterrupted(timed_out, timed_out ? "Nix exceeded buildTimeoutSeconds" : "Nix cancelled");
    } catch (const nix::BaseError& e) {
      if (impl_->interrupted(call, nullptr)) {
        bool timed_out = false;
        impl_->interrupted(call, &timed_out);
        throw NixInterrupted(timed_out, timed_out ? "Nix exceeded buildTimeoutSeconds" : "Nix cancelled");
      }
      throw std::runtime_error(message(e));
    }
  });
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

}  // namespace tsugumi
