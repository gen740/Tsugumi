#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "card_call.hh"
#include "execution_service.hh"
#include "service.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/workerd/artifact_id.hh"
#include "tsugumi/workerd/frame.hh"

namespace tsugumi {
namespace {
namespace C = Tsugumi::Card;
namespace fsys = std::filesystem;
using Clock = std::chrono::steady_clock;
using Tick = Clock::duration::rep;
using namespace std::chrono_literals;

constexpr Tick kNoDeadline = Clock::duration::max().count();
constexpr auto kDeadlineGrace = 500ms;

auto from_abi(const auto& uuid) -> ViewUuid {
  ViewUuid result;
  std::ranges::copy(uuid.bytes, result.begin());
  return result;
}

auto to_abi(const ViewUuid& uuid, auto& out) -> void {
  std::ranges::copy(uuid, out.bytes);
}

auto path_bytes(const std::string& path) -> tsugumi_bytes {
  return {reinterpret_cast<const uint8_t*>(path.c_str()), path.size()};
}

struct ActiveCall {
  explicit ActiveCall(const ExecutionRequest& request)
      : artifacts(request.directory / "packed-artifacts") {}

  ~ActiveCall() {
    if (!has_artifacts) return;
    try {
      fs::clear_staging(artifacts);
      fsys::remove(artifacts);
    } catch (const std::exception& error) {
      std::fprintf(stderr, "Cannot clear packed artifact staging: %s\n",
                   error.what());
    }
  }

  fsys::path artifacts;
  bool has_artifacts = false;
  pack::ResultMemory result;
  std::atomic_flag result_requested;
  std::mutex artifacts_mutex;
  std::vector<CreatedArtifact> created;
  std::map<ArtifactKey, std::string> opened;
  std::deque<std::string> paths;
};

struct DeadlineWatch {
  std::mutex mutex;
  std::condition_variable changed;
};

class FastCard {
 public:
  FastCard(Service& service, const std::string& library, DeadlineWatch& watch)
      : service_(service),
        watch_(watch),
        image_(library),
        receiver_(std::make_shared<WorkerProc::ViewReceiver>()) {
    receiver_->remote = [this](const WorkerViewEvent& event,
                               WorkerProc::DisplayCredit credit) {
      std::lock_guard lock(view_mutex_);
      if (closed_) return WorkerProc::ViewDelivery::closed;
      const auto cost = workerd::message_cost(event.bytes.size());
      if (event.kind == ViewEventKind::message) {
        if (cost > workerd::kViewBudget - incoming_bytes_)
          return WorkerProc::ViewDelivery::full;
        incoming_bytes_ += cost;
      }
      incoming_.emplace_back(event, std::move(credit));
      view_ready_.notify_all();
      return WorkerProc::ViewDelivery::accepted;
    };
  }

  auto close() -> void {
    {
      std::lock_guard lock(view_mutex_);
      closed_ = true;
    }
    view_ready_.notify_all();
    receiver_->close();
  }

  // Called by the watcher under its mutex.
  auto check_deadline(Tick now) const -> Tick {
    if (hard_deadline_ != kNoDeadline && now >= hard_deadline_) {
      // A Card that has stopped responding can also hold stdio's locks.
      (void)::write(STDERR_FILENO, deadline_message_.data(),
                    deadline_message_.size());
      std::_Exit(124);
    }
    return hard_deadline_;
  }

  auto run(const ExecutionRequest& request, bool verify) -> ExecutionResult {
    if (deadline_message_.empty()) {
      deadline_message_ = "Packed Card exceeded its hard deadline: " +
                          path_key(request.node.path) + "\n";
    }
    {
      std::lock_guard lock(view_mutex_);
      view_context_ = std::make_shared<const ViewContext>(
          Service::ViewOrigin(request.summary), request.build.views);
    }
    request.summary.execution_service = kLocalExecutionService;
    request.start({});
    std::vector<tsugumi_input> inputs;
    inputs.reserve(request.inputs.size());
    for (const auto& value : request.inputs)
      inputs.push_back({value.name.c_str(),
                        value.schema.c_str(),
                        {reinterpret_cast<const uint8_t*>(value.bytes.data()),
                         value.bytes.size()}});
    std::vector<pack::OutputPort> outputs;
    outputs.reserve(request.node.outputs.size());
    for (const auto& port : request.node.outputs)
      outputs.push_back({port.name, port.required});

    ActiveCall active(request);
    const auto begin = now_ms();
    std::shared_ptr<const pack::CardResult> result;
    std::optional<FailureData> failure;
    {
      CallbackScope scope(*this, active, request);
      const tsugumi_call call{TSUGUMI_ABI_VERSION, inputs.data(), inputs.size(),
                              this, &kEffects};
      try {
        result = std::make_shared<pack::CardResult>(
            image_.call(call, active.result, outputs, verify));
      } catch (const std::runtime_error& error) {
        const auto kind = cancelled_.load()  ? FailureKind::cancelled
                          : remaining() == 0 ? FailureKind::timeout
                                             : FailureKind::output_invalid;
        failure = FailureData::make(kind, error.what());
      }
    }
    if (request.on_timing) request.on_timing(begin, now_ms());
    if (result) {
      if (const auto* error = result->root().failure()) {
        auto kind = error->kind();
        if (kind == FailureKind::unspecified ||
            !*Tsugumi::Data::EnumNameFailureKind(kind))
          kind = FailureKind::worker_failed;
        failure = FailureData::make(
            kind, error->message() ? error->message()->str() : "");
      }
    }
    try {
      publish_artifacts(request, active);
    } catch (const std::exception&) {
      if (!failure) throw;
    }
    if (failure) return {{}, std::move(failure)};
    std::vector<RawOutput> raw;
    if (const auto* ports = result->root().outputs()) {
      raw.reserve(ports->size());
      for (const auto* port : *ports) {
        std::string_view bytes;
        if (const auto* span = port->bytes())
          bytes = {reinterpret_cast<const char*>(span->data()), span->size()};
        raw.push_back(
            {port->name()->str(), PayloadBytes::borrow(bytes, result)});
      }
    }
    workerd::InvocationResult completed{
        workerd::accept_outputs(std::move(raw), request.node.outputs), {}};
    if (request.validate) request.validate(completed);
    return {std::move(completed.outputs), {}};
  }

 private:
  struct ViewContext {
    Service::ViewOrigin origin;
    std::vector<schema::CardView> views;
  };

  class CallbackScope {
   public:
    CallbackScope(FastCard& card, ActiveCall& active,
                  const ExecutionRequest& request)
        : card_(card), stop_(request.stop, [&card] {
            {
              std::lock_guard lock(card.view_mutex_);
              card.cancelled_ = true;
            }
            card.view_ready_.notify_all();
          }) {
      card_.cancelled_ = request.cancel.load() || request.stop.stop_requested();
      if (request.node.settings.timeout_seconds) {
        const auto now = Clock::now();
        const auto limit = Clock::time_point::max() - now - kDeadlineGrace;
        const auto seconds = std::min<uint64_t>(
            *request.node.settings.timeout_seconds,
            std::chrono::duration_cast<std::chrono::seconds>(limit).count());
        const auto deadline = now + std::chrono::seconds(seconds);
        card_.deadline_ = deadline.time_since_epoch().count();
        card_.set_hard_deadline(
            (deadline + kDeadlineGrace).time_since_epoch().count());
      }
      card_.active_ = &active;
    }

    ~CallbackScope() {
      card_.set_hard_deadline(kNoDeadline);
      card_.deadline_ = kNoDeadline;
      card_.active_ = nullptr;
      card_.cancelled_ = false;
    }

   private:
    FastCard& card_;
    std::stop_callback<std::function<void()>> stop_;
  };

  auto set_hard_deadline(Tick deadline) -> void {
    std::lock_guard lock(watch_.mutex);
    hard_deadline_ = deadline;
    watch_.changed.notify_one();
  }

  auto remaining() const -> int64_t {
    const auto deadline = deadline_.load(std::memory_order_relaxed);
    if (deadline == kNoDeadline) return -1;
    const auto time =
        Clock::time_point(Clock::duration(deadline)) - Clock::now();
    return std::max<int64_t>(
        0, std::chrono::ceil<std::chrono::milliseconds>(time).count());
  }

  auto control(WorkerViewRequest request) -> WorkerViewReply {
    std::shared_ptr<const ViewContext> context;
    {
      std::lock_guard lock(view_mutex_);
      if (closed_) return {0};
      context = view_context_;
    }
    if (!context) return {0};
    request.request_id = 1;
    return service_.view_control(context->origin, context->views, receiver_,
                                 request);
  }

  auto send(const tsugumi_view_session* session, const uint8_t* bytes,
            size_t size) -> int {
    if (!session || (size != 0 && !bytes) || size > TSUGUMI_MESSAGE_MAX)
      return -1;
    const auto cost = workerd::message_cost(size);
    {
      std::unique_lock lock(view_mutex_);
      view_ready_.wait(lock, [&] {
        return closed_ || cancelled_ ||
               workerd::kViewBudget - outgoing_bytes_ >= cost;
      });
      if (closed_ || cancelled_) return -1;
      outgoing_bytes_ += cost;
    }
    auto payload = size
                       ? std::string(reinterpret_cast<const char*>(bytes), size)
                       : std::string{};
    service_.display({from_abi(*session), std::move(payload)}, [this, cost] {
      {
        std::lock_guard lock(view_mutex_);
        outgoing_bytes_ -= cost;
      }
      view_ready_.notify_all();
    });
    return 0;
  }

  auto receive(tsugumi_view_event* out, int64_t milliseconds) -> int {
    if (!out) return -1;
    std::unique_lock lock(view_mutex_);
    const auto ready = [&] {
      return closed_ || cancelled_ || !incoming_.empty();
    };
    if (milliseconds < 0)
      view_ready_.wait(lock, ready);
    else if (!view_ready_.wait_for(
                 lock, std::chrono::milliseconds(milliseconds), ready))
      return 1;
    if (closed_ || cancelled_) return 2;
    auto [event, credit] = std::move(incoming_.front());
    incoming_.pop_front();
    if (event.kind == ViewEventKind::message)
      incoming_bytes_ -= workerd::message_cost(event.bytes.size());
    lock.unlock();
    if (credit) credit();
    thread_local std::string received;
    received = std::move(event.bytes);
    out->kind = event.kind == ViewEventKind::close ? 1 : 0;
    to_abi(event.route, out->route);
    to_abi(event.session, out->session);
    out->data = path_bytes(received);
    return 0;
  }

  auto create_artifact(uint32_t flags, tsugumi_artifact_id* out,
                       tsugumi_bytes* path) -> int {
    auto* active = active_.load();
    if (!active || !out || !path || (flags & ~TSUGUMI_ARTIFACT_KEEP)) return -1;
    std::lock_guard lock(active->artifacts_mutex);
    auto id = *parse_artifact_id(crypto::uuid4());
    const auto directory =
        active->artifacts / "outputs" / std::to_string(active->created.size());
    active->has_artifacts = true;
    fsys::create_directories(directory);
    active->paths.push_back(directory.string());
    std::ranges::copy(id, out->bytes);
    *path = path_bytes(active->paths.back());
    active->created.push_back(
        {std::move(id), bool(flags & TSUGUMI_ARTIFACT_KEEP)});
    return 0;
  }

  auto open_artifact(const tsugumi_artifact_id* value, tsugumi_bytes* path)
      -> int {
    auto* active = active_.load();
    if (!active || !value || !path) return -1;
    std::lock_guard lock(active->artifacts_mutex);
    const std::string id(reinterpret_cast<const char*>(value->bytes), 16);
    const auto prior = active->opened.find(id);
    if (prior != active->opened.end()) {
      *path = path_bytes(prior->second);
      return 0;
    }
    const auto resolved = service_.resolve_artifact(id);
    if (resolved.empty()) return -1;
    // Provide the same directory shape as a worker invocation. A Card can
    // copy it with metadata without inheriting the store's sealed directories.
    const auto input = active->artifacts / "inputs" / artifact_id_text(id);
    active->has_artifacts = true;
    fsys::create_directories(input.parent_path());
    fs::link_artifact(resolved, input);
    *path =
        path_bytes(active->opened.emplace(id, input.string()).first->second);
    return 0;
  }

  auto publish_artifacts(const ExecutionRequest& request, ActiveCall& active)
      -> void {
    if (active.created.empty()) return;
    const auto target = service_.artifact_destination(request.summary);
    const auto outputs = active.artifacts / "outputs";
    const int directory = ::open(
        outputs.c_str(), O_DIRECTORY | O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0)
      throw Failed(FailureKind::output_invalid,
                   "Cannot open packed artifact staging");
    struct Staging {
      int fd;
      ~Staging() { ::close(fd); }
    } stage{directory};
    fs::OutputBudget budget{request.node.settings.output_limit_bytes,
                            request.node.settings.output_limit_entries};
    size_t published = 0;
    try {
      for (; published < active.created.size(); ++published) {
        const auto name = std::to_string(published);
        fs::move_staging_artifact(stage.fd, name, target / name, budget);
      }
    } catch (const std::exception& error) {
      service_.record_artifacts(request.summary,
                                std::span(active.created).first(published));
      throw Failed(FailureKind::output_invalid, error.what());
    }
    service_.record_artifacts(request.summary, active.created);
  }

  template <typename Function>
  static auto effect(Function operation) -> int {
    try {
      return operation();
    } catch (const std::exception&) {
      return -1;
    }
  }

  static auto self(void* context) -> FastCard& {
    return *static_cast<FastCard*>(context);
  }

  static const tsugumi_effects kEffects;

  Service& service_;
  DeadlineWatch& watch_;
  pack::CardImage image_;
  const std::shared_ptr<WorkerProc::ViewReceiver> receiver_;
  std::string deadline_message_;
  std::atomic<ActiveCall*> active_{nullptr};
  std::atomic<Tick> deadline_{kNoDeadline};
  Tick hard_deadline_ = kNoDeadline;  // guarded by watch_.mutex
  std::atomic<bool> cancelled_{false};
  std::atomic<bool> closed_{false};
  std::mutex view_mutex_;
  // As with a resident worker, effects between calls use the last invocation.
  std::shared_ptr<const ViewContext> view_context_;
  std::condition_variable view_ready_;
  std::deque<std::pair<WorkerViewEvent, WorkerProc::DisplayCredit>> incoming_;
  size_t incoming_bytes_ = 0;
  size_t outgoing_bytes_ = 0;
};

const tsugumi_effects FastCard::kEffects{
    .view_open = [](void* context, const char* name, uint32_t mode,
                    uint32_t flags, uint32_t width, uint32_t height,
                    tsugumi_route* route,
                    tsugumi_view_session* session) -> int {
      if (!name || !route || !session) return -1;
      return effect([&] {
        const auto result = self(context).control(
            {0, ViewControlKind::open, name, mode, flags, width, height});
        if (result.status == 0) {
          to_abi(result.route, *route);
          to_abi(result.session, *session);
        }
        return result.status;
      });
    },
    .view_close = [](void* context,
                     const tsugumi_view_session* session) -> int {
      if (!session) return -1;
      return effect([&] {
        return self(context)
            .control({.request_id = 0,
                      .kind = ViewControlKind::close,
                      .view = {},
                      .session = from_abi(*session)})
            .status;
      });
    },
    .route_register = [](void* context, const tsugumi_view_session* session,
                         tsugumi_route* route) -> int {
      if (!session || !route) return -1;
      return effect([&] {
        const auto result =
            self(context).control({.request_id = 0,
                                   .kind = ViewControlKind::register_route,
                                   .view = {},
                                   .session = from_abi(*session)});
        if (result.status == 0) to_abi(result.route, *route);
        return result.status;
      });
    },
    .route_unregister = [](void* context, const tsugumi_route* route) -> int {
      if (!route) return -1;
      return effect([&] {
        return self(context)
            .control({.request_id = 0,
                      .kind = ViewControlKind::unregister_route,
                      .view = {},
                      .route = from_abi(*route)})
            .status;
      });
    },
    .send = [](void* context, const tsugumi_view_session* session,
               const uint8_t* bytes, size_t size) -> int {
      return effect([&] { return self(context).send(session, bytes, size); });
    },
    .recv = [](void* context, tsugumi_view_event* out, int64_t timeout) -> int {
      return effect([&] { return self(context).receive(out, timeout); });
    },
    .cancelled = [](void* context) -> int {
      return self(context).cancelled_.load(std::memory_order_relaxed);
    },
    .remaining_ms = [](void* context) { return self(context).remaining(); },
    .artifact_create = [](void* context, uint32_t flags,
                          tsugumi_artifact_id* id, tsugumi_bytes* path) -> int {
      return effect(
          [&] { return self(context).create_artifact(flags, id, path); });
    },
    .artifact_open = [](void* context, const tsugumi_artifact_id* id,
                        tsugumi_bytes* path) -> int {
      return effect([&] { return self(context).open_artifact(id, path); });
    },
    .result_alloc = [](void* context, size_t size) -> uint8_t* {
      auto* active = self(context).active_.load(std::memory_order_relaxed);
      if (!active || active->result_requested.test_and_set()) return nullptr;
      auto* memory = active->result.allocate(size);
      if (!memory) active->result_requested.clear();
      return memory;
    },
};

struct Runtime {
  DeadlineWatch deadlines;
  std::map<std::string, std::unique_ptr<FastCard>, std::less<>> cards;
  std::jthread watchdog;
};

// A packed process serves one app. The loaded images and their callback
// contexts must remain valid through process exit, including Card threads.
auto runtime() -> Runtime& {
  static auto* instance = new Runtime;
  return *instance;
}

class PreparedFastExecution final : public PreparedExecution {
 public:
  explicit PreparedFastExecution(bool verify) : verify_(verify) {}

  auto execute(const ExecutionRequest& request) -> ExecutionResult override {
    const auto library =
        (fsys::path(request.build.package) / "lib/main.so").string();
    const auto& card = runtime().cards.at(library);
    return card->run(request, verify_);
  }

 private:
  bool verify_;
};
}  // namespace

auto load_fast_pack_images(Service& service,
                           std::span<const std::string> libraries) -> void {
  auto& pack = runtime();
  if (pack.watchdog.joinable()) {
    pack.watchdog.request_stop();
    pack.watchdog.join();
  }
  for (const auto& library : libraries)
    if (!pack.cards.contains(library))
      pack.cards.emplace(library, std::make_unique<FastCard>(service, library,
                                                             pack.deadlines));
  pack.watchdog = std::jthread([&pack](std::stop_token stop) {
    std::stop_callback wake(stop, [&pack] {
      std::lock_guard lock(pack.deadlines.mutex);
      pack.deadlines.changed.notify_all();
    });
    std::unique_lock lock(pack.deadlines.mutex);
    while (!stop.stop_requested()) {
      const auto now = Clock::now().time_since_epoch().count();
      auto next = kNoDeadline;
      for (const auto& [_, card] : pack.cards) {
        const auto deadline = card->check_deadline(now);
        if (deadline != kNoDeadline && (next == kNoDeadline || deadline < next))
          next = deadline;
      }
      if (next == kNoDeadline)
        pack.deadlines.changed.wait(lock);
      else
        pack.deadlines.changed.wait_until(
            lock, Clock::time_point(Clock::duration(next)));
    }
  });
}

auto stop_fast_pack_images(Service&) -> void {
  auto& pack = runtime();
  // A hard deadline still applies while the scheduler is waiting for a Card.
  for (const auto& [_, card] : pack.cards) card->close();
}

auto prepare_fast_pack_execution(Service& service, const Node&)
    -> std::unique_ptr<PreparedExecution> {
  return std::make_unique<PreparedFastExecution>(service.packed_debug());
}

}  // namespace tsugumi
