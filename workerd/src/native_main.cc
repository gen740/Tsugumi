// The Card host exchanges call and result frames on fd 3 and receives
// cancellation and reverse view frames on fd 4.
#include <dlfcn.h>
#include <fcntl.h>
#include <flatbuffers/flatbuffers.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "display_channel.hh"
#include "tsugumi/card_abi.h"
#include "tsugumi/protocol/card_generated.h"
#include "tsugumi/protocol/worker_generated.h"
#include "tsugumi/rpc/frame.hh"
#include "tsugumi/workerd/frame.hh"

namespace C = Tsugumi::Card;
namespace D = Tsugumi::Data;
namespace W = Tsugumi::Worker;
namespace frame = tsugumi::rpc::frame;
namespace relay = tsugumi::workerd;
using tsugumi::ViewControlKind;
using tsugumi::ViewEventKind;
using tsugumi::ViewUuid;
using tsugumi::WorkerViewEvent;
using tsugumi::WorkerViewReply;
using tsugumi::WorkerViewRequest;
using Clock = std::chrono::steady_clock;

namespace {

constexpr size_t kMaxFrame = tsugumi::workerd::kMaxWorkerFrameBytes;
constexpr int kData = 3;
constexpr int kControl = 4;
using tsugumi::workerd::kViewBudget;
using tsugumi::workerd::message_cost;
constexpr size_t kMaxViewName = kMaxFrame - TSUGUMI_MESSAGE_MAX - 1024;
using Main = decltype(&tsugumi_main);
using ArtifactId = std::array<uint8_t, 16>;


struct ViewQueue {
  std::deque<std::string> outgoing;
  size_t outgoing_bytes = 0;
  size_t credit = kViewBudget;
  uint64_t queued_messages = 0;
  uint64_t forwarded_messages = 0;
};

struct RouteQueue {
  size_t incoming_bytes = 0;
  bool retired = false;
  bool closed = false;
};

struct Host {
  std::mutex write_mutex;
  std::mutex view_mutex;
  std::condition_variable view_ready;
  std::map<ViewUuid, ViewQueue> views;
  std::map<ViewUuid, RouteQueue> routes;
  std::map<ViewUuid, size_t> returned_credits;
  std::deque<WorkerViewEvent> incoming;
  std::mutex request_mutex;
  uint64_t request_id = 0;
  std::optional<ViewUuid> unregistering;
  std::optional<WorkerViewReply> reply;
  bool stopping = false;
  std::mutex artifact_mutex;
  std::string artifact_directory;
  std::map<ArtifactId, std::string> opened;
  std::map<ArtifactId, std::string> created;
  std::atomic<uint64_t> cancelled_id{0};
  std::atomic<uint64_t> call_id{0};
  std::atomic<Clock::duration::rep> deadline{Clock::duration::max().count()};
};

struct Failure {
  D::FailureKind kind;
  std::string message;
  std::vector<std::pair<std::string, std::string>> args = {};
};

auto read_frame(int fd) -> std::optional<std::string> {
  try {
    return frame::Read(fd, kMaxFrame);
  } catch (const frame::Error&) {
    return std::nullopt;
  }
}

auto send_frame(Host& host, const flatbuffers::FlatBufferBuilder& b) -> bool {
  std::lock_guard guard(host.write_mutex);
  try {
    frame::Write(
        kData,
        {reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize()},
        kMaxFrame);
    return true;
  } catch (const frame::Error&) {
    return false;
  }
}

auto interrupted(const Host& host, uint64_t call) -> bool {
  return host.stopping || (call != 0 && host.cancelled_id.load() == call);
}

auto abi_uuid(const uint8_t (&bytes)[16]) -> ViewUuid {
  ViewUuid id;
  std::ranges::copy(bytes, id.begin());
  return id;
}

auto request_view(Host& host, WorkerViewRequest request)
    -> std::optional<WorkerViewReply> {
  try {
    std::lock_guard request_lock(host.request_mutex);
    {
      std::lock_guard lock(host.view_mutex);
      if (host.stopping) return {};
      request.request_id = ++host.request_id;
      if (request.kind == ViewControlKind::unregister_route)
        host.unregistering = request.route;
    }
    flatbuffers::FlatBufferBuilder b;
    relay::build_view_request(b, request);
    if (!send_frame(host, b)) {
      std::lock_guard lock(host.view_mutex);
      host.stopping = true;
      host.view_ready.notify_all();
      return {};
    }
    std::unique_lock lock(host.view_mutex);
    host.view_ready.wait(lock, [&] { return host.stopping || host.reply; });
    return std::exchange(host.reply, std::nullopt);
  } catch (const std::exception&) {
    return {};
  }
}

auto open_view(void* context, const char* view, uint32_t mode, uint32_t flags,
               uint32_t width, uint32_t height, tsugumi_route* route,
               tsugumi_view_session* session) -> int {
  if (!view || !route || !session || mode > TSUGUMI_VIEW_FULLSCREEN ||
      (flags & ~(TSUGUMI_VIEW_HIDE_CURSOR | TSUGUMI_VIEW_LOCK_POINTER)) ||
      ((width == 0) != (height == 0)) ||
      (width != 0 && mode != TSUGUMI_VIEW_WINDOW))
    return -1;
  const auto size = ::strnlen(view, kMaxViewName);
  if (size == 0 || size == kMaxViewName) return -1;
  try {
    const auto reply =
        request_view(*static_cast<Host*>(context),
                     {0, ViewControlKind::open, std::string(view, size), mode,
                      flags, width, height});
    if (!reply || reply->status != 0 || reply->route == ViewUuid{} ||
        reply->session == ViewUuid{})
      return -1;
    std::ranges::copy(reply->route, route->bytes);
    std::ranges::copy(reply->session, session->bytes);
    return 0;
  } catch (const std::exception&) {
    return -1;
  }
}

auto close_view(void* context, const tsugumi_view_session* session) -> int {
  if (!session) return -1;
  const auto id = abi_uuid(session->bytes);
  if (id == ViewUuid{}) return -1;
  const auto reply = request_view(*static_cast<Host*>(context),
                                  {.request_id = 0,
                                   .kind = ViewControlKind::close,
                                   .view = {},
                                   .session = id});
  return reply && reply->status == 0 ? 0 : -1;
}

auto register_route(void* context, const tsugumi_view_session* session,
                    tsugumi_route* route) -> int {
  if (!session || !route) return -1;
  const auto reply = request_view(*static_cast<Host*>(context),
                                  {.request_id = 0,
                                   .kind = ViewControlKind::register_route,
                                   .view = {},
                                   .session = abi_uuid(session->bytes)});
  if (!reply) return -1;
  if (reply->status == TSUGUMI_VIEW_WINDOW_CLOSED) return reply->status;
  if (reply->status != 0 || reply->route == ViewUuid{}) return -1;
  std::ranges::copy(reply->route, route->bytes);
  return 0;
}

auto unregister_route(void* context, const tsugumi_route* route) -> int {
  if (!route) return -1;
  const auto reply = request_view(*static_cast<Host*>(context),
                                  {.request_id = 0,
                                   .kind = ViewControlKind::unregister_route,
                                   .view = {},
                                   .route = abi_uuid(route->bytes)});
  return reply && reply->status == 0 ? 0 : -1;
}

auto send_display(void* context, const tsugumi_view_session* session,
                  const uint8_t* bytes, size_t size) -> int {
  if (size > TSUGUMI_MESSAGE_MAX || !session || (!bytes && size != 0))
    return -1;
  const auto id = abi_uuid(session->bytes);
  if (id == ViewUuid{}) return -1;
  auto& host = *static_cast<Host*>(context);
  try {
    std::unique_lock lock(host.view_mutex);
    auto& queue = host.views[id];
    const auto call = host.call_id.load();
    host.view_ready.wait(lock, [&] {
      return interrupted(host, call) ||
             queue.outgoing_bytes <= kViewBudget - TSUGUMI_MESSAGE_MAX;
    });
    if (interrupted(host, call)) return -1;
    queue.outgoing.emplace_back(bytes ? reinterpret_cast<const char*>(bytes)
                                      : "", size);
    queue.outgoing_bytes += message_cost(size);
    ++queue.queued_messages;
    host.view_ready.notify_all();
    return 0;
  } catch (const std::exception&) {
    return -1;
  }
}

auto receive_display(void* context, tsugumi_view_event* out, int64_t timeout_ms)
    -> int {
  thread_local std::string received;
  received.clear();
  if (!out) return -1;
  *out = {};
  auto& host = *static_cast<Host*>(context);
  try {
    std::unique_lock lock(host.view_mutex);
    const auto call = host.call_id.load();
    const auto ready = [&] {
      return interrupted(host, call) || !host.incoming.empty();
    };
    if (timeout_ms < 0) {
      host.view_ready.wait(lock, ready);
    } else {
      const auto now = Clock::now();
      const int64_t maximum =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              Clock::time_point::max() - now).count();
      if (!host.view_ready.wait_until(
              lock, now + std::chrono::milliseconds(std::min(timeout_ms, maximum)),
              ready))
        return 1;
    }
    if (interrupted(host, call)) return 2;
    auto event = std::move(host.incoming.front());
    host.incoming.pop_front();
    if (event.kind == ViewEventKind::message) {
      const auto cost = message_cost(event.bytes.size());
      host.routes.at(event.route).incoming_bytes -= cost;
      host.returned_credits[event.route] += cost;
      host.view_ready.notify_all();
    }
    received = std::move(event.bytes);
    out->kind = static_cast<uint32_t>(event.kind);
    std::ranges::copy(event.route, out->route.bytes);
    std::ranges::copy(event.session, out->session.bytes);
    if (event.kind == ViewEventKind::message)
      out->data = {reinterpret_cast<const uint8_t*>(received.data()),
                   received.size()};
    return 0;
  } catch (const std::exception&) {
    return -1;
  }
}

auto display_loop(Host& host) -> void {
  const auto ready = [](const auto& entry) {
    const auto& queue = entry.second;
    return !queue.outgoing.empty() && queue.credit >= TSUGUMI_MESSAGE_MAX;
  };
  ViewUuid previous{};
  for (;;) {
    flatbuffers::FlatBufferBuilder b;
    ViewQueue* forwarded = nullptr;
    {
      std::unique_lock lock(host.view_mutex);
      host.view_ready.wait(lock, [&] {
        return host.stopping || !host.returned_credits.empty() ||
               std::ranges::any_of(host.views, ready);
      });
      if (host.stopping) return;
      if (!host.returned_credits.empty()) {
        auto credit =
            host.returned_credits.extract(host.returned_credits.begin());
        relay::build_display_credit(b, credit.key(), credit.mapped());
      } else {
        const auto end = host.views.end();
        auto next = std::find_if(host.views.upper_bound(previous), end, ready);
        if (next == end) next = std::find_if(host.views.begin(), end, ready);
        auto& [session, queue] = *next;
        previous = session;
        const auto& bytes = queue.outgoing.front();
        relay::build_display(b, session, bytes);
        queue.credit -= message_cost(bytes.size());
        queue.outgoing_bytes -= message_cost(bytes.size());
        queue.outgoing.pop_front();
        forwarded = &queue;
      }
      host.view_ready.notify_all();
    }
    const bool sent = send_frame(host, b);
    {
      std::lock_guard lock(host.view_mutex);
      if (!sent) host.stopping = true;
      if (sent && forwarded) ++forwarded->forwarded_messages;
      host.view_ready.notify_all();
    }
    if (!sent) return;
  }
}

// The result can retire a non-resident worker. Put all messages already
// submitted by this call onto the data channel before it returns its result.
auto flush_display(Host& host, uint64_t call) -> void {
  std::unique_lock lock(host.view_mutex);
  std::vector<std::pair<const ViewQueue*, uint64_t>> pending;
  for (const auto& [name, queue] : host.views)
    if (queue.forwarded_messages < queue.queued_messages)
      pending.emplace_back(&queue, queue.queued_messages);
  host.view_ready.wait(lock, [&] {
    return interrupted(host, call) ||
           std::ranges::all_of(pending, [](const auto& entry) {
             return entry.first->forwarded_messages >= entry.second;
           });
  });
}

auto cancelled(void* context) -> int {
  const auto& host = *static_cast<Host*>(context);
  const auto call = host.call_id.load();
  return call != 0 && host.cancelled_id.load() == call;
}

auto remaining_ms(void* context) -> int64_t {
  const auto deadline =
      Clock::duration(static_cast<Host*>(context)->deadline.load());
  if (deadline == Clock::duration::max()) return -1;
  const auto remaining = Clock::time_point(deadline) - Clock::now();
  return remaining <= Clock::duration::zero()
             ? 0
             : std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
}

auto path_bytes(const std::string& path) -> tsugumi_bytes {
  return {reinterpret_cast<const uint8_t*>(path.c_str()), path.size()};
}

auto create_artifact(void* context, uint32_t flags, tsugumi_artifact_id* out,
                     tsugumi_bytes* path) -> int {
  if (!out || !path) return -1;
  *path = {};
  auto& host = *static_cast<Host*>(context);
  try {
    std::lock_guard lock(host.artifact_mutex);
    const auto call_id = host.call_id.load();
    if (!call_id || host.artifact_directory.empty()) return -1;
    flatbuffers::FlatBufferBuilder b;
    const auto request = W::CreateWorkerArtifactCreate(b, flags);
    W::FinishWorkerMessageBuffer(
        b, W::CreateWorkerMessage(b, call_id,
                                  W::WorkerPayload::WorkerArtifactCreate,
                                  request.Union()));
    if (!send_frame(host, b)) return -1;
    const auto reply = read_frame(kData);
    if (!reply) return -1;
    flatbuffers::Verifier verify(
        reinterpret_cast<const uint8_t*>(reply->data()), reply->size());
    if (!W::VerifyWorkerMessageBuffer(verify)) return -1;
    const auto* message = W::GetWorkerMessage(reply->data());
    const auto* created = message->value_as_WorkerArtifactCreated();
    if (message->call_id() != call_id || !created || !created->id()) return -1;
    const auto directory =
        host.artifact_directory + "/" + std::to_string(created->counter());
    if (::mkdir(directory.c_str(), 0700) != 0) return -1;
    ArtifactId key;
    std::ranges::copy(*created->id()->bytes(), key.begin());
    const auto [entry, inserted] = host.created.emplace(key, directory);
    if (!inserted) return -1;
    std::ranges::copy(key, out->bytes);
    *path = path_bytes(entry->second);
    return 0;
  } catch (const std::exception&) {
    return -1;
  }
}

auto open_artifact(void* context, const tsugumi_artifact_id* id,
                   tsugumi_bytes* path) -> int {
  if (!id || !path) return -1;
  *path = {};
  auto& host = *static_cast<Host*>(context);
  try {
    std::lock_guard lock(host.artifact_mutex);
    const auto call_id = host.call_id.load();
    if (!call_id) return -1;
    ArtifactId key;
    std::ranges::copy(id->bytes, key.begin());
    if (const auto found = host.opened.find(key); found != host.opened.end()) {
      *path = path_bytes(found->second);
      return 0;
    }
    flatbuffers::FlatBufferBuilder b;
    const tsugumi::ArtifactId artifact{flatbuffers::span(key)};
    const auto request = W::CreateWorkerArtifactOpen(b, &artifact);
    W::FinishWorkerMessageBuffer(
        b,
        W::CreateWorkerMessage(b, call_id, W::WorkerPayload::WorkerArtifactOpen,
                               request.Union()));
    if (!send_frame(host, b)) return -1;
    const auto reply = read_frame(kData);
    if (!reply) return -1;
    flatbuffers::Verifier verify(
        reinterpret_cast<const uint8_t*>(reply->data()), reply->size());
    if (!W::VerifyWorkerMessageBuffer(verify)) return -1;
    const auto* message = W::GetWorkerMessage(reply->data());
    const auto* opened = message->value_as_WorkerArtifactOpened();
    if (message->call_id() != call_id || !opened || !opened->path()) return -1;
    *path = path_bytes(
        host.opened.emplace(key, opened->path()->str()).first->second);
    return 0;
  } catch (const std::exception&) {
    return -1;
  }
}

constexpr tsugumi_effects kEffects{
    open_view,       close_view,      register_route, unregister_route,
    send_display,    receive_display, cancelled,      remaining_ms,
    create_artifact, open_artifact};

auto control_loop(Host& host) -> void {
  while (auto bytes = read_frame(kControl)) {
    flatbuffers::Verifier verifier(
        reinterpret_cast<const uint8_t*>(bytes->data()), bytes->size());
    if (!W::VerifyWorkerMessageBuffer(verifier)) break;
    const auto* message = W::GetWorkerMessage(bytes->data());
    std::lock_guard lock(host.view_mutex);
    if (message->value_as_WorkerCancel() && message->call_id()) {
      host.cancelled_id =
          std::max(host.cancelled_id.load(), *message->call_id());
    } else if (const auto* data = message->value_as_WorkerViewEvent();
               data && message->call_id() == 0) {
      auto event = relay::view_event(*data);
      if (!relay::valid_view_event(event)) break;
      auto& route = host.routes[event.route];
      if (event.kind == ViewEventKind::close) {
        if (!route.retired && !std::exchange(route.closed, true))
          host.incoming.push_back(std::move(event));
      } else if (route.retired || route.closed) {
        host.returned_credits[event.route] += message_cost(event.bytes.size());
      } else {
        if (route.incoming_bytes > kViewBudget - TSUGUMI_MESSAGE_MAX) break;
        route.incoming_bytes += message_cost(event.bytes.size());
        host.incoming.push_back(std::move(event));
      }
    } else if (const auto* reply = message->value_as_WorkerViewReply();
               reply && message->call_id() == 0) {
      if (reply->request_id() != host.request_id || host.reply) break;
      if (host.unregistering && reply->status() == 0) {
        auto& route = host.routes[*host.unregistering];
        route.retired = true;
        if (route.incoming_bytes)
          host.returned_credits[*host.unregistering] +=
              std::exchange(route.incoming_bytes, 0);
        std::erase_if(host.incoming, [&](const auto& event) {
          return event.route == *host.unregistering;
        });
      }
      host.unregistering.reset();
      host.reply = relay::view_reply(*reply);
    } else if (const auto* credit = message->value_as_WorkerDisplayCredit();
               credit && message->call_id() == 0) {
      const auto found = host.views.find(relay::view_uuid(credit->address()));
      if (found == host.views.end() ||
          credit->bytes() > kViewBudget - found->second.credit)
        break;
      found->second.credit += credit->bytes();
    } else {
      break;
    }
    host.view_ready.notify_all();
  }
  std::lock_guard lock(host.view_mutex);
  host.stopping = true;
  host.view_ready.notify_all();
}

auto load_card(const std::string& package, std::string& error)
    -> Main {
  const auto file = package + "/lib/main.so";
  // Retain the image for the lifetime of Card threads and the effects table.
  void* image = ::dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!image) {
    error = ::dlerror();
    return nullptr;
  }
  auto entry =
      reinterpret_cast<Main>(::dlsym(image, "tsugumi_main"));
  if (!entry) error = "main.so does not export tsugumi_main";
  return entry;
}

// FlatBuffers tracks alignment relative to the end of its backing store.
class InputAllocator final : public flatbuffers::Allocator {
 public:
  auto allocate(size_t size) -> uint8_t* override {
    void* data = nullptr;
    if (posix_memalign(&data, FLATBUFFERS_MAX_ALIGNMENT, size) != 0)
      throw std::bad_alloc();
    return static_cast<uint8_t*>(data);
  }

  auto deallocate(uint8_t* data, size_t) -> void override { std::free(data); }
};

using Payload = flatbuffers::Vector<uint8_t>;

auto aligned_payload(flatbuffers::FlatBufferBuilder& b, const Payload& data)
    -> flatbuffers::Offset<Payload> {
  b.ForceVectorAlignment(data.size(), sizeof(uint8_t),
                         FLATBUFFERS_MAX_ALIGNMENT);
  return b.CreateVector(data.data(), data.size());
}

auto failure_frame(flatbuffers::FlatBufferBuilder& b, uint64_t id,
                   const Failure& failure) -> void {
  std::vector<flatbuffers::Offset<D::KeyValue>> args;
  for (const auto& [key, value] : failure.args)
    args.push_back(
        D::CreateKeyValue(b, b.CreateString(key), b.CreateString(value)));
  const auto encoded = D::CreateFailure(b, failure.kind, b.CreateVector(args),
                                        b.CreateString(failure.message));
  const auto payload = W::CreateWorkerFailure(b, encoded);
  W::FinishWorkerMessageBuffer(
      b, W::CreateWorkerMessage(b, id, W::WorkerPayload::WorkerFailure,
                                payload.Union()));
}

auto card_failure(const C::CardFailure& value) -> Failure {
  auto kind = value.kind();
  if (kind == D::FailureKind::unspecified || !*D::EnumNameFailureKind(kind))
    kind = D::FailureKind::worker_failed;
  const auto text = [](const flatbuffers::String* field) -> std::string {
    return field ? std::string(field->string_view().substr(0, 4096)) : "";
  };
  Failure failure{kind, text(value.message())};
  if (value.source()) failure.args.emplace_back("source", text(value.source()));
  if (value.line())
    failure.args.emplace_back("line", std::to_string(value.line()));
  if (value.cell()) failure.args.emplace_back("cell", text(value.cell()));
  return failure;
}

auto make_result(const C::CardResult& result, uint64_t id,
                 flatbuffers::FlatBufferBuilder& b) -> std::optional<Failure> {
  if (const auto* failure = result.failure()) return card_failure(*failure);
  std::vector<flatbuffers::Offset<W::WorkerOutput>> outputs;
  if (const auto* ports = result.outputs()) {
    for (const auto* output : *ports) {
      const auto name = output->name()->string_view();
      flatbuffers::Offset<Payload> bytes;
      if (const auto* value = output->bytes())
        bytes = aligned_payload(b, *value);
      outputs.push_back(W::CreateWorkerOutput(b, b.CreateString(name), bytes));
      if (b.GetSize() > kMaxFrame)
        return Failure{D::FailureKind::output_invalid,
                       "outputs exceed the worker frame limit"};
    }
  }
  const auto payload = W::CreateWorkerResult(b, b.CreateVector(outputs));
  W::FinishWorkerMessageBuffer(
      b, W::CreateWorkerMessage(b, id, W::WorkerPayload::WorkerResult,
                                payload.Union()));
  if (b.GetSize() > kMaxFrame)
    return Failure{D::FailureKind::output_invalid,
                   "outputs exceed the worker frame limit"};
  return std::nullopt;
}

auto execute(const W::WorkerCall& call, Host& host, uint64_t id,
             const std::string& package, Main& entry,
             std::string& load_error, flatbuffers::FlatBufferBuilder& reply)
    -> std::optional<Failure> {
  const auto stop = [&]() -> std::optional<Failure> {
    if (cancelled(&host))
      return Failure{D::FailureKind::cancelled, "the invocation was cancelled"};
    if (remaining_ms(&host) == 0)
      return Failure{D::FailureKind::timeout,
                     "the Card exceeded timeoutSeconds"};
    return std::nullopt;
  };
  // The SDK borrows this envelope and its aligned payloads for the call.
  InputAllocator allocator;
  flatbuffers::FlatBufferBuilder b(1024, &allocator, false,
                                   FLATBUFFERS_MAX_ALIGNMENT);
  std::vector<flatbuffers::Offset<C::CardPort>> inputs;
  for (const auto* input : *call.inputs()) {
    flatbuffers::Offset<Payload> bytes;
    if (const auto* payload = input->bytes()) bytes = aligned_payload(b, *payload);
    const auto schema = input->schema() ? b.CreateString(input->schema()->string_view())
                                        : flatbuffers::Offset<flatbuffers::String>{};
    inputs.push_back(C::CreateCardPort(
        b, b.CreateString(input->name()->string_view()), schema, bytes));
  }
  b.TrackMinAlign(FLATBUFFERS_MAX_ALIGNMENT);
  b.Finish(C::CreateCardInput(b, b.CreateVector(inputs)));
  {
    std::lock_guard lock(host.artifact_mutex);
    host.created.clear();
    host.opened.clear();
    host.artifact_directory.clear();
    if (const auto* directory = call.artifact_directory())
      host.artifact_directory = directory->str();
  }

  if (auto failure = stop()) return failure;
  if (!entry && load_error.empty()) entry = load_card(package, load_error);
  if (!entry) return Failure{D::FailureKind::worker_failed, load_error};
  if (auto failure = stop()) return failure;
  const tsugumi_call abi{TSUGUMI_ABI_VERSION,
                         {b.GetBufferPointer(), b.GetSize()},
                         &host,
                         &kEffects};
  tsugumi_bytes returned{};
  const int status = entry(&abi, &returned);
  const std::unique_ptr<void, decltype(&std::free)> result_owner(
      const_cast<uint8_t*>(returned.data), &std::free);
  if (auto failure = stop()) return failure;
  if (status != 0)
    return Failure{D::FailureKind::worker_failed,
                   "tsugumi_main returned " + std::to_string(status)};
  if (!returned.data || returned.size > kMaxFrame)
    return Failure{D::FailureKind::output_invalid, "invalid CardResult length"};
  flatbuffers::Verifier verifier(returned.data, returned.size);
  if (!verifier.VerifyBuffer<C::CardResult>(nullptr))
    return Failure{D::FailureKind::output_invalid, "invalid CardResult buffer"};
  return make_result(*flatbuffers::GetRoot<C::CardResult>(returned.data), id,
                     reply);
}

auto serve(const std::string& package) -> int {
  // This context lives as long as the process, including when Card threads
  // enter send or recv after a call returns.
  Host host;
  std::thread(control_loop, std::ref(host)).detach();
  std::thread(display_loop, std::ref(host)).detach();
  Main entry = nullptr;
  std::string load_error;
  uint64_t last_call = 0;
  bool started = false;
  while (auto frame = read_frame(kData)) {
    flatbuffers::Verifier verifier(
        reinterpret_cast<const uint8_t*>(frame->data()), frame->size());
    if (!W::VerifyWorkerMessageBuffer(verifier)) std::_Exit(2);
    const auto* message = W::GetWorkerMessage(frame->data());
    if (const auto* start = message->value_as_WorkerStart();
        start && !started) {
      started = true;
      bool success = true;
      for (const auto* env : *start->environment())
        success &=
            ::setenv(env->name()->c_str(), env->value()->c_str(), 1) == 0;
      tsugumi::workerd::clear_secret(*frame);
      if (!success) std::_Exit(2);
      continue;
    }
    started = true;
    const auto* call = message->value_as_WorkerCall();
    if (!call || !message->call_id() || *message->call_id() <= last_call)
      std::_Exit(2);
    last_call = *message->call_id();
    const auto now = Clock::now();
    const auto maximum = std::chrono::duration_cast<std::chrono::milliseconds>(
                             Clock::time_point::max() - now)
                             .count();
    const auto delay = std::min(call->deadline_ms().value_or(UINT64_MAX),
                                static_cast<uint64_t>(maximum));
    host.deadline = call->deadline_ms()
                        ? (now + std::chrono::milliseconds(delay))
                              .time_since_epoch()
                              .count()
                        : Clock::duration::max().count();
    host.call_id = last_call;
    flatbuffers::FlatBufferBuilder reply;
    if (const auto failure = execute(*call, host, last_call, package, entry,
                                     load_error, reply)) {
      reply.Clear();
      failure_frame(reply, last_call, *failure);
    }
    bool drain = call->drain_display();
    {
      std::lock_guard lock(host.artifact_mutex);
      host.call_id = 0;
      drain |= !host.created.empty();
    }
    if (drain) flush_display(host, last_call);
    std::fflush(stdout);
    std::fflush(stderr);
    if (!send_frame(host, reply)) break;
  }
  std::_Exit(0);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3 || std::string_view(argv[1]) != "--package") return 2;
  ::signal(SIGPIPE, SIG_IGN);
  // Card subprocesses use stdout/stderr but do not inherit the ABI sockets.
  ::fcntl(kData, F_SETFD, FD_CLOEXEC);
  ::fcntl(kControl, F_SETFD, FD_CLOEXEC);
  return serve(argv[2]);
}
