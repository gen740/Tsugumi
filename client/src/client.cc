#include "tsugumi/client/client.hh"

#include <flatbuffers/verifier.h>

#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

#include "tsugumi/protocol/agent_generated.h"
#include "tsugumi/protocol/host_generated.h"
// musl exposes these C streams as macros; the wire schema uses the same names
// as scoped enum values. Keep the workaround local to generated declarations.
#pragma push_macro("stdout")
#pragma push_macro("stderr")
#undef stdout
#undef stderr
#include "tsugumi/protocol/tsugumi_generated.h"
#pragma pop_macro("stderr")
#pragma pop_macro("stdout")

namespace tsugumi::client {
namespace {

enum class Kind { Request, Reply, Event };
struct Envelope {
  Kind kind;
  uint64_t id;
};

template <class Tag>
auto KnownPayload(Tag tag) -> bool {
  return tag > Tag::NONE && tag <= Tag::MAX;
}

auto Inspect(Protocol protocol, std::span<const uint8_t> bytes) -> Envelope {
  flatbuffers::Verifier verifier(bytes.data(), bytes.size());
  if (protocol == Protocol::Project) {
    namespace R = Tsugumi::RPC;
    if (!R::VerifyMessageBuffer(verifier))
      throw std::runtime_error("invalid project RPC buffer");
    const auto* message = R::GetMessage(bytes.data());
    const auto id = message->request_id().value_or(0);
    switch (message->payload_type()) {
      case R::Payload::Request:
        if (id && message->payload_as_Request() &&
            KnownPayload(message->payload_as_Request()->value_type()))
          return {Kind::Request, id};
        break;
      case R::Payload::Reply:
        if (id && message->payload_as_Reply() &&
            KnownPayload(message->payload_as_Reply()->value_type()))
          return {Kind::Reply, id};
        break;
      case R::Payload::Notification:
        if (!id && message->payload_as_Notification() &&
            KnownPayload(message->payload_as_Notification()->value_type()))
          return {Kind::Event, 0};
        break;
      default:
        break;
    }
  } else if (protocol == Protocol::Agent) {
    namespace A = Tsugumi::Agent;
    if (!A::VerifyMessageBuffer(verifier))
      throw std::runtime_error("invalid Agent RPC buffer");
    const auto* message = A::GetMessage(bytes.data());
    const auto id = message->request_id();
    switch (message->payload_type()) {
      case A::Payload::Request:
        if (id && message->payload_as_Request() &&
            KnownPayload(message->payload_as_Request()->value_type()))
          return {Kind::Request, id};
        break;
      case A::Payload::Reply:
        if (id && message->payload_as_Reply() &&
            KnownPayload(message->payload_as_Reply()->value_type()))
          return {Kind::Reply, id};
        break;
      default:
        break;
      }
  } else if (protocol == Protocol::Host) {
    namespace A = Tsugumi::Host;
    if (!A::VerifyMessageBuffer(verifier))
      throw std::runtime_error("invalid Host RPC buffer");
    const auto *message = A::GetMessage(bytes.data());
    const auto id = message->request_id();
    switch (message->payload_type()) {
    case A::Payload::Request:
      if (id && message->payload_as_Request() &&
          KnownPayload(message->payload_as_Request()->value_type()))
        return {Kind::Request, id};
      break;
    case A::Payload::Reply:
      if (id && message->payload_as_Reply() &&
          KnownPayload(message->payload_as_Reply()->value_type()))
        return {Kind::Reply, id};
      break;
    default:
      break;
    }
  }
  throw std::runtime_error("invalid RPC envelope");
}

auto Invoke(const Connection::Completion& callback, Result result) -> void {
  if (callback) callback(std::move(result));
}

}  // namespace

struct Connection::State {
  struct Pending {
    Completion callback;
    std::chrono::steady_clock::time_point deadline;
  };
  explicit State(Options value) : options(std::move(value)) {}
  Options options;
  std::mutex mutex;
  bool started = false;
  bool opened = false;
  bool closed = false;
  uint64_t next_id = 1;
  std::map<uint64_t, Pending> pending;
  std::function<void(Error)> start_callback;
  std::function<void(Error)> close_callback;
  std::function<void(Bytes)> event_callback;
  std::weak_ptr<Transport> transport;

  auto End(Error error) -> void {
    std::map<uint64_t, Pending> failed;
    std::function<void(Error)> start;
    std::function<void(Error)> close;
    {
      std::lock_guard lock(mutex);
      if (closed) return;
      closed = true;
      opened = false;
      failed.swap(pending);
      start = std::exchange(start_callback, {});
      close = close_callback;
    }
    if (start) start(error);
    for (auto& [id, request] : failed) Invoke(request.callback, {{}, error});
    if (close) close(std::move(error));
  }

  auto Opened() -> void {
    std::function<void(Error)> callback;
    {
      std::lock_guard lock(mutex);
      if (closed) return;
      opened = true;
      callback = std::exchange(start_callback, {});
    }
    if (callback) callback({});
  }

  auto Tick() -> void {
    std::vector<Completion> expired;
    const auto now = std::chrono::steady_clock::now();
    {
      std::lock_guard lock(mutex);
      for (auto it = pending.begin(); it != pending.end();) {
        if (it->second.deadline > now) {
          ++it;
          continue;
        }
        expired.push_back(std::move(it->second.callback));
        it = pending.erase(it);
      }
    }
    for (const auto& callback : expired)
      Invoke(callback, {{}, {ErrorCode::Timeout, "RPC request timed out"}});
  }

  auto Receive(Bytes bytes) -> void {
    Envelope envelope;
    try {
      if (bytes.size() > options.max_frame_bytes)
        throw std::runtime_error("RPC message too large");
      envelope = Inspect(options.protocol, bytes);
      if (envelope.kind == Kind::Request)
        throw std::runtime_error("server sent a request");
    } catch (const std::exception& error) {
      End({ErrorCode::Protocol, error.what()});
      if (auto current = transport.lock()) current->Close();
      return;
    }
    Completion callback;
    std::function<void(Bytes)> event;
    bool unknown = false;
    {
      std::lock_guard lock(mutex);
      if (closed) return;
      if (envelope.kind == Kind::Event) {
        event = event_callback;
      } else if (auto found = pending.find(envelope.id);
                 found != pending.end()) {
        callback = std::move(found->second.callback);
        pending.erase(found);
      } else {
        // A timed-out reply is harmless. An ID never issued is a protocol
        // error.
        unknown = envelope.id >= next_id;
      }
    }
    if (unknown) {
      End({ErrorCode::Protocol, "reply to an unissued request ID"});
      if (auto current = transport.lock()) current->Close();
    } else if (callback) {
      Invoke(callback, {std::move(bytes), {}});
    } else if (event) {
      event(std::move(bytes));
    }
  }

  auto Submit(BuildRequest build, std::optional<Bytes> supplied,
              Completion completion, std::chrono::milliseconds timeout)
      -> uint64_t {
    Bytes frame;
    uint64_t id = 0;
    Error error;
    {
      std::lock_guard lock(mutex);
      if (closed || !opened)
        error = {ErrorCode::Closed, "RPC transport is not open"};
      else if (timeout.count() <= 0)
        error = {ErrorCode::InvalidArgument, "timeout must be positive"};
      else if (pending.size() >= options.max_pending_requests)
        error = {ErrorCode::Backpressure, "too many pending RPC requests"};
      else {
        try {
          id = next_id;
          if (!id || id == UINT64_MAX)
            throw std::runtime_error("request IDs exhausted; reconnect");
          frame = supplied ? std::move(*supplied) : build(id);
          if (frame.size() > options.max_frame_bytes)
            throw std::runtime_error("RPC request too large");
          const auto envelope = Inspect(options.protocol, frame);
          if (envelope.kind != Kind::Request || envelope.id < next_id ||
              (!supplied && envelope.id != id) || envelope.id == UINT64_MAX)
            throw std::runtime_error(
                "request ID must be nonzero and strictly increasing");
          id = envelope.id;
          next_id = id + 1;
          pending.emplace(
              id,
              Pending{completion, std::chrono::steady_clock::now() + timeout});
          // Assignment and enqueue share the lock: concurrent callers cannot
          // put request 2 on the wire before request 1.
          if (auto current = transport.lock())
            error = current->Send(std::move(frame));
          else
            error = {ErrorCode::Closed, "RPC transport was destroyed"};
          if (error) pending.erase(id);
        } catch (const std::exception& failure) {
          pending.erase(id);
          error = {ErrorCode::InvalidArgument, failure.what()};
        }
      }
    }
    if (error) {
      Invoke(completion, {{}, std::move(error)});
      return 0;
    }
    return id;
  }
};

Connection::Connection(Options options)
    : Connection(options, MakeTransport(options)) {}
Connection::Connection(Options options, std::shared_ptr<Transport> transport)
    : state_(std::make_shared<State>(std::move(options))),
      transport_(std::move(transport)) {
  if (!transport_) throw std::invalid_argument("transport is required");
  state_->transport = transport_;
}
Connection::~Connection() { Close(); }

auto Connection::Start(std::function<void(Error)> completion) -> void {
  bool invalid;
  {
    std::lock_guard lock(state_->mutex);
    invalid = state_->started || state_->closed;
    if (!invalid) {
      state_->started = true;
      state_->start_callback = completion;
    }
  }
  if (invalid) {
    if (completion)
      completion({ErrorCode::InvalidArgument, "connection already started"});
    return;
  }
  const std::weak_ptr<State> weak = state_;
  try {
    transport_->Start({[weak] {
                         if (auto state = weak.lock()) state->Opened();
                       },
                       [weak](Bytes bytes) {
                         if (auto state = weak.lock())
                           state->Receive(std::move(bytes));
                       },
                       [weak](Error error) {
                         if (auto state = weak.lock())
                           state->End(std::move(error));
                       },
                       [weak] {
                         if (auto state = weak.lock()) state->Tick();
                       }});
  } catch (const std::exception& error) {
    state_->End({ErrorCode::Transport, error.what()});
  }
}

auto Connection::Start() -> std::future<Error> {
  auto promise = std::make_shared<std::promise<Error>>();
  auto future = promise->get_future();
  Start([promise](Error error) { promise->set_value(std::move(error)); });
  return future;
}

auto Connection::OnEvent(std::function<void(Bytes)> callback) -> void {
  std::lock_guard lock(state_->mutex);
  state_->event_callback = std::move(callback);
}
auto Connection::OnClosed(std::function<void(Error)> callback) -> void {
  std::lock_guard lock(state_->mutex);
  state_->close_callback = std::move(callback);
}
auto Connection::Call(BuildRequest build, Completion callback,
                      std::chrono::milliseconds timeout) -> uint64_t {
  return state_->Submit(std::move(build), {}, std::move(callback), timeout);
}
auto Connection::Call(BuildRequest build, std::chrono::milliseconds timeout)
    -> std::future<Result> {
  auto promise = std::make_shared<std::promise<Result>>();
  auto future = promise->get_future();
  Call(
      std::move(build),
      [promise](Result result) { promise->set_value(std::move(result)); },
      timeout);
  return future;
}
auto Connection::CallFrame(Bytes frame, Completion callback,
                           std::chrono::milliseconds timeout) -> void {
  state_->Submit({}, std::move(frame), std::move(callback), timeout);
}
auto Connection::CallFrame(Bytes frame, std::chrono::milliseconds timeout)
    -> std::future<Result> {
  auto promise = std::make_shared<std::promise<Result>>();
  auto future = promise->get_future();
  CallFrame(
      std::move(frame),
      [promise](Result result) { promise->set_value(std::move(result)); },
      timeout);
  return future;
}
auto Connection::Close() -> void {
  auto state = state_;
  auto transport = transport_;
  state->End({ErrorCode::Closed, "RPC connection closed"});
  transport->Close();
}

}  // namespace tsugumi::client
