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
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "tsugumi/card_abi.h"
#include "tsugumi/protocol/card_generated.h"
#include "tsugumi/protocol/worker_generated.h"
#include "tsugumi/rpc/frame.hh"
#include "tsugumi/workerd/frame.hh"

namespace C = Tsugumi::Card;
namespace D = Tsugumi::Data;
namespace W = Tsugumi::Worker;
namespace frame = tsugumi::rpc::frame;
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
using ArtifactIds = flatbuffers::Vector<const tsugumi::ArtifactId*>;

auto id_bytes(const tsugumi::ArtifactId& id) -> ArtifactId {
  ArtifactId result;
  std::ranges::copy(*id.bytes(), result.begin());
  return result;
}

auto id_hex(const ArtifactId& id) -> std::string {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  for (const auto byte : id) {
    result += digits[byte >> 4];
    result += digits[byte & 15];
  }
  return result;
}

struct ViewQueue {
  std::deque<std::string> incoming;
  std::deque<std::string> outgoing;
  size_t incoming_bytes = 0;
  size_t outgoing_bytes = 0;
  size_t credit = kViewBudget;
  size_t returned_credit = 0;
  uint64_t queued_messages = 0;
  uint64_t forwarded_messages = 0;
};

struct Host {
  std::mutex write_mutex;
  std::mutex view_mutex;
  std::condition_variable view_ready;
  std::map<std::string, ViewQueue, std::less<>> views;
  bool stopping = false;
  std::mutex artifact_mutex;
  std::string artifact_directory;
  std::map<ArtifactId, std::string> granted;
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

auto send_display(void* context, const char* view, const uint8_t* bytes,
                  size_t size) -> int {
  if (size > TSUGUMI_MESSAGE_MAX || !view || (!bytes && size != 0)) return -1;
  const auto name_size = ::strnlen(view, kMaxViewName);
  if (name_size == 0 || name_size == kMaxViewName) return -1;
  auto& host = *static_cast<Host*>(context);
  try {
    std::unique_lock lock(host.view_mutex);
    auto& queue = host.views[std::string(view, name_size)];
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

auto receive_display(void* context, const char* view, tsugumi_bytes* out,
                     int64_t timeout_ms) -> int {
  thread_local std::string received;
  received.clear();
  if (!out || !view) return -1;
  *out = {};
  const auto name_size = ::strnlen(view, kMaxViewName);
  if (name_size == 0 || name_size == kMaxViewName) return -1;
  auto& host = *static_cast<Host*>(context);
  try {
    std::unique_lock lock(host.view_mutex);
    auto& queue = host.views[std::string(view, name_size)];
    const auto call = host.call_id.load();
    const auto ready = [&] {
      return interrupted(host, call) || !queue.incoming.empty();
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
    received = std::move(queue.incoming.front());
    queue.incoming.pop_front();
    queue.incoming_bytes -= message_cost(received.size());
    queue.returned_credit += message_cost(received.size());
    host.view_ready.notify_all();
    *out = {reinterpret_cast<const uint8_t*>(received.data()), received.size()};
    return 0;
  } catch (const std::exception&) {
    return -1;
  }
}

auto display_loop(Host& host) -> void {
  const auto ready = [](const auto& entry) {
    const auto& queue = entry.second;
    return queue.returned_credit != 0 ||
           (!queue.outgoing.empty() && queue.credit >= TSUGUMI_MESSAGE_MAX);
  };
  std::string previous;
  for (;;) {
    flatbuffers::FlatBufferBuilder b;
    ViewQueue* forwarded = nullptr;
    {
      std::unique_lock lock(host.view_mutex);
      host.view_ready.wait(lock, [&] {
        return host.stopping || std::ranges::any_of(host.views, ready);
      });
      if (host.stopping) return;
      const auto end = host.views.end();
      auto next = std::find_if(host.views.upper_bound(previous), end, ready);
      if (next == end) next = std::find_if(host.views.begin(), end, ready);
      auto& [view, queue] = *next;
      previous = view;
      const auto name = b.CreateString(view);
      if (queue.returned_credit) {
        const auto update = W::CreateWorkerDisplayCredit(
            b, name, static_cast<uint32_t>(queue.returned_credit));
        queue.returned_credit = 0;
        W::FinishWorkerMessageBuffer(b, W::CreateWorkerMessage(
            b, 0, W::WorkerPayload::WorkerDisplayCredit, update.Union()));
      } else {
        const auto& bytes = queue.outgoing.front();
        const auto update = W::CreateWorkerDisplay(
            b, name, b.CreateVector(reinterpret_cast<const uint8_t*>(bytes.data()),
                                     bytes.size()));
        queue.credit -= message_cost(bytes.size());
        queue.outgoing_bytes -= message_cost(bytes.size());
        queue.outgoing.pop_front();
        forwarded = &queue;
        W::FinishWorkerMessageBuffer(b, W::CreateWorkerMessage(
            b, 0, W::WorkerPayload::WorkerDisplay, update.Union()));
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

auto create_artifact(void* context, tsugumi_artifact_id* out,
                     tsugumi_bytes* path) -> int {
  if (!out || !path) return -1;
  *path = {};
  auto& host = *static_cast<Host*>(context);
  try {
    std::lock_guard lock(host.artifact_mutex);
    if (host.artifact_directory.empty()) return -1;
    ArtifactId id;
#ifdef __APPLE__
    ::arc4random_buf(id.data(), id.size());
#else
    if (::getentropy(id.data(), id.size()) != 0) return -1;
#endif
    const auto directory = host.artifact_directory + "/" + id_hex(id);
    if (host.granted.contains(id) || host.created.contains(id) ||
        ::mkdir(directory.c_str(), 0700) != 0)
      return -1;
    const auto inserted = host.created.emplace(id, directory);
    std::ranges::copy(id, out->bytes);
    *path = path_bytes(inserted.first->second);
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
  std::lock_guard lock(host.artifact_mutex);
  ArtifactId key;
  std::ranges::copy(id->bytes, key.begin());
  const auto found = host.granted.find(key);
  if (found == host.granted.end()) return -1;
  struct stat info{};
  if (::lstat(found->second.c_str(), &info) != 0 || !S_ISDIR(info.st_mode))
    return -1;
  *path = path_bytes(found->second);
  return 0;
}

constexpr tsugumi_effects kEffects{
    send_display, receive_display, cancelled, remaining_ms,
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
    } else if (const auto* data = message->value_as_WorkerDisplay();
               data && message->call_id() == 0) {
      if (data->view()->size() == 0 || data->view()->size() >= kMaxViewName ||
          data->bytes()->size() > TSUGUMI_MESSAGE_MAX)
        break;
      auto& queue = host.views[data->view()->str()];
      if (queue.incoming_bytes > kViewBudget - TSUGUMI_MESSAGE_MAX) break;
      queue.incoming.emplace_back(data->bytes()->begin(), data->bytes()->end());
      queue.incoming_bytes += message_cost(data->bytes()->size());
    } else if (const auto* credit = message->value_as_WorkerDisplayCredit();
               credit && message->call_id() == 0) {
      const auto found = host.views.find(credit->view()->string_view());
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

using Payload = flatbuffers::Vector<uint8_t>;

auto aligned_payload(flatbuffers::FlatBufferBuilder& b, const Payload& data)
    -> flatbuffers::Offset<Payload> {
  b.ForceVectorAlignment(data.size(), sizeof(uint8_t),
                         FLATBUFFERS_MAX_ALIGNMENT);
  return b.CreateVector(data.data(), data.size());
}

auto failure_frame(flatbuffers::FlatBufferBuilder& b, uint64_t id,
                   const Failure& failure,
                   const std::vector<tsugumi::ArtifactId>& created) -> void {
  std::vector<flatbuffers::Offset<D::KeyValue>> args;
  for (const auto& [key, value] : failure.args)
    args.push_back(
        D::CreateKeyValue(b, b.CreateString(key), b.CreateString(value)));
  const auto encoded = D::CreateFailure(b, failure.kind, b.CreateVector(args),
                                        b.CreateString(failure.message));
  const auto payload = W::CreateWorkerFailure(b, encoded, b.CreateVectorOfStructs(created));
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

auto created_ids(const Host& host) -> std::vector<tsugumi::ArtifactId> {
  std::vector<tsugumi::ArtifactId> created;
  for (const auto& entry : host.created)
    created.emplace_back(flatbuffers::span<const uint8_t, 16>(entry.first));
  return created;
}

auto copy_ids(flatbuffers::FlatBufferBuilder& b, const ArtifactIds* ids) {
  std::vector<tsugumi::ArtifactId> copy;
  if (ids)
    for (const auto* id : *ids) copy.push_back(*id);
  return b.CreateVectorOfStructs(copy);
}

auto make_result(const C::CardResult& result, const Host& host, uint64_t id,
                 flatbuffers::FlatBufferBuilder& b) -> std::optional<Failure> {
  if (const auto* failure = result.failure()) return card_failure(*failure);
  std::vector<flatbuffers::Offset<W::WorkerOutput>> outputs;
  if (const auto* ports = result.outputs()) {
    for (const auto* output : *ports) {
      const auto name = output->name()->string_view();
      flatbuffers::Offset<Payload> bytes;
      if (const auto* value = output->bytes())
        bytes = aligned_payload(b, *value);
      outputs.push_back(W::CreateWorkerOutput(
          b, b.CreateString(name), bytes, copy_ids(b, output->artifacts())));
      if (b.GetSize() > kMaxFrame)
        return Failure{D::FailureKind::output_invalid,
                       "outputs exceed the worker frame limit"};
    }
  }
  const auto payload = W::CreateWorkerResult(
      b, b.CreateVector(outputs), b.CreateVectorOfStructs(created_ids(host)));
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
  // Input bytes belong to the Card's SDK: they are copied
  // aligned, never opened.
  flatbuffers::FlatBufferBuilder b;
  std::vector<flatbuffers::Offset<C::CardPort>> inputs;
  std::set<ArtifactId> requested;
  for (const auto* input : *call.inputs()) {
    flatbuffers::Offset<Payload> bytes;
    if (const auto* payload = input->bytes()) bytes = aligned_payload(b, *payload);
    if (const auto* ids = input->artifacts())
      for (const auto* id : *ids) requested.insert(id_bytes(*id));
    const auto schema = input->schema() ? b.CreateString(input->schema()->string_view())
                                        : flatbuffers::Offset<flatbuffers::String>{};
    inputs.push_back(C::CreateCardPort(
        b, b.CreateString(input->name()->string_view()), schema, bytes,
        copy_ids(b, input->artifacts())));
  }
  b.Finish(C::CreateCardInput(b, b.CreateVector(inputs)));
  {
    std::lock_guard lock(host.artifact_mutex);
    host.created.clear();
    host.granted.clear();
    host.artifact_directory.clear();
    if (const auto* directory = call.artifact_directory())
      host.artifact_directory = directory->str();
    if (const auto* grants = call.grants()) {
      for (const auto* grant : *grants) {
        const auto key = id_bytes(*grant->id());
        if (!requested.erase(key))
          return Failure{D::FailureKind::input_missing, "unexpected artifact grant"};
        host.granted.emplace(key, grant->path()->str());
      }
    }
    if (!requested.empty())
      return Failure{D::FailureKind::input_missing, "an input artifact is unavailable"};
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
  if (auto failure = stop()) return failure;
  if (status != 0)
    return Failure{D::FailureKind::worker_failed,
                   "tsugumi_main returned " + std::to_string(status)};
  if (!returned.data || returned.size > kMaxFrame)
    return Failure{D::FailureKind::output_invalid, "invalid CardResult length"};
  flatbuffers::Verifier verifier(returned.data, returned.size);
  if (!verifier.VerifyBuffer<C::CardResult>(nullptr))
    return Failure{D::FailureKind::output_invalid, "invalid CardResult buffer"};
  return make_result(*flatbuffers::GetRoot<C::CardResult>(returned.data), host,
                     id, reply);
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
      failure_frame(reply, last_call, *failure, created_ids(host));
    }
    if (call->drain_display()) flush_display(host, last_call);
    host.call_id = 0;
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
