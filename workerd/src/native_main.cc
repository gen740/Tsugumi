// The Card host exchanges call and result frames on fd 3 and receives
// cancellation on fd 4. A Card can send opaque view data on fd 3 at any time
// after receiving the effects table.
#include <dlfcn.h>
#include <fcntl.h>
#include <flatbuffers/reflection.h>
#include <signal.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
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

struct Host {
  std::mutex write_mutex;
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

auto send_display(void* context, const char* view, const uint8_t* bytes,
                  size_t size) -> int {
  if (!view || (!bytes && size != 0) || size > kMaxFrame) return 1;
  const auto name_size = ::strnlen(view, kMaxFrame);
  if (name_size >= kMaxFrame || size > kMaxFrame - name_size) return 1;
  try {
    flatbuffers::FlatBufferBuilder b;
    const auto update = W::CreateWorkerDisplay(
        b, b.CreateString(view, name_size), b.CreateVector(bytes, size));
    W::FinishWorkerMessageBuffer(
        b, W::CreateWorkerMessage(b, 0, W::WorkerPayload::WorkerDisplay,
                                  update.Union()));
    return send_frame(*static_cast<Host*>(context), b) ? 0 : 1;
  } catch (const std::exception&) {
    return 1;
  }
}

auto cancelled(void* context) -> int {
  const auto& host = *static_cast<Host*>(context);
  return host.cancelled_id.load() == host.call_id.load();
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

constexpr tsugumi_effects kEffects{send_display, cancelled, remaining_ms};

auto control_loop(Host& host) -> void {
  while (auto bytes = read_frame(kControl)) {
    flatbuffers::Verifier verifier(
        reinterpret_cast<const uint8_t*>(bytes->data()), bytes->size());
    if (!W::VerifyWorkerMessageBuffer(verifier)) continue;
    const auto* message = W::GetWorkerMessage(bytes->data());
    if (message->value_as_WorkerCancel() && message->call_id())
      host.cancelled_id =
          std::max(host.cancelled_id.load(), *message->call_id());
  }
}

auto load_card(const std::string& package, std::string& error)
    -> tsugumi_main_fn {
  const auto file = package + "/lib/main.so";
  // Retain the image for the lifetime of Card threads and the effects table.
  void* image = ::dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!image) {
    error = ::dlerror();
    return nullptr;
  }
  auto entry =
      reinterpret_cast<tsugumi_main_fn>(::dlsym(image, "tsugumi_main"));
  if (!entry) error = "main.so does not export tsugumi_main";
  return entry;
}

using Payload = flatbuffers::Vector<uint8_t>;

struct MessageType {
  const reflection::Schema* schema = nullptr;
  const reflection::Object* root = nullptr;

  MessageType(const Payload* bfbs, const flatbuffers::String* name) {
    if (!bfbs || !name) return;
    flatbuffers::Verifier verifier(bfbs->data(), bfbs->size());
    if (!reflection::VerifySchemaBuffer(verifier)) return;
    schema = reflection::GetSchema(bfbs->data());
    root = schema->objects()->LookupByKey(name->c_str());
  }

  auto verifies(const Payload* payload) const -> bool {
    return root && !root->is_struct() && payload &&
           flatbuffers::Verify(*schema, *root, payload->data(),
                               payload->size());
  }

  // The File and Directory contracts carry the path in a string named value.
  auto path(const Payload* payload) const -> const flatbuffers::String* {
    const auto* field = root->fields()->LookupByKey("value");
    if (!field || field->type()->base_type() != reflection::String)
      return nullptr;
    return flatbuffers::GetFieldS(*flatbuffers::GetAnyRoot(payload->data()),
                                  *field);
  }
};

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

auto make_result(const C::CardResult& result, const W::WorkerCall& call,
                 const std::map<std::pair<D::PortKind, std::string_view>,
                                const flatbuffers::String*>& input_paths,
                 uint64_t id, flatbuffers::FlatBufferBuilder& b)
    -> std::optional<Failure> {
  if (const auto* failure = result.failure()) return card_failure(*failure);
  std::map<std::string_view, const D::Port*> missing;
  for (const auto* port : *call.outputs())
    missing.emplace(port->name()->string_view(), port);
  std::vector<flatbuffers::Offset<W::WorkerOutput>> outputs;
  if (const auto* ports = result.outputs()) {
    for (const auto* output : *ports) {
      const auto name = output->name()->string_view();
      const auto found = missing.find(name);
      const auto* port = found == missing.end() ? nullptr : found->second;
      if (!port || output->kind() != port->kind())
        return Failure{
            D::FailureKind::output_invalid,
            "unknown, duplicate, or wrong-kind output " + std::string(name),
            {{"port", std::string(name)}}};
      const auto* payload = output->bytes();
      const MessageType type(port->schema_bfbs(), port->root_type());
      if (!type.verifies(payload))
        return Failure{
            D::FailureKind::output_invalid,
            "output " + std::string(name) + " does not match its type",
            {{"port", std::string(name)}}};
      flatbuffers::Offset<flatbuffers::String> forwarded;
      if (port->kind() == D::PortKind::File ||
          port->kind() == D::PortKind::Directory) {
        const auto* path = type.path(payload);
        if (!path || path->size() == 0)
          return Failure{D::FailureKind::output_invalid,
                         "output has no path",
                         {{"port", std::string(name)}}};
        if (const auto in =
                input_paths.find({port->kind(), path->string_view()});
            in != input_paths.end())
          forwarded = b.CreateString(in->second->string_view());
      }
      const auto bytes = aligned_payload(b, *payload);
      outputs.push_back(
          W::CreateWorkerOutput(b, b.CreateString(name), bytes, forwarded));
      missing.erase(found);
      if (b.GetSize() > kMaxFrame)
        return Failure{D::FailureKind::output_invalid,
                       "outputs exceed the worker frame limit"};
    }
  }
  for (const auto& [name, port] : missing)
    if (!port->optional().value_or(false))
      return Failure{D::FailureKind::output_invalid,
                     "required output " + std::string(name) + " is missing",
                     {{"port", std::string(name)}}};
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
             const std::string& package, tsugumi_main_fn& entry,
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
  flatbuffers::FlatBufferBuilder b;
  std::vector<flatbuffers::Offset<C::CardPort>> inputs;
  std::map<std::pair<D::PortKind, std::string_view>, const flatbuffers::String*>
      input_paths;
  for (const auto* input : *call.inputs()) {
    const auto* value = input->value();
    const MessageType type(value->schema_bfbs(), value->root_type());
    if (!type.verifies(value->bytes()))
      return Failure{
          D::FailureKind::type_mismatch,
          "input " + input->name()->str() + " does not match its type"};
    if (input->kind() == D::PortKind::File ||
        input->kind() == D::PortKind::Directory)
      if (const auto* path = type.path(value->bytes()))
        input_paths.emplace(std::pair{input->kind(), path->string_view()},
                            input->name());
    const auto bytes = aligned_payload(b, *value->bytes());
    inputs.push_back(C::CreateCardPort(
        b, b.CreateString(input->name()->string_view()), input->kind(), bytes));
  }
  const auto* params = call.params();
  const MessageType params_type(params->schema_bfbs(), params->root_type());
  if (!params_type.verifies(params->bytes()))
    return Failure{D::FailureKind::type_mismatch,
                   "parameters do not match their type"};
  const auto payload = aligned_payload(b, *params->bytes());
  b.Finish(C::CreateCardInput(b, b.CreateVector(inputs), payload));

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
  return make_result(*flatbuffers::GetRoot<C::CardResult>(returned.data), call,
                     input_paths, id, reply);
}

auto serve(const std::string& package) -> int {
  // A process-lifetime host context. serve exits the process without running
  // static destructors while Card threads can still enter send.
  Host host;
  std::thread(control_loop, std::ref(host)).detach();
  tsugumi_main_fn entry = nullptr;
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
