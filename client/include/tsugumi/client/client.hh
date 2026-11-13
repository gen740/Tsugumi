#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace tsugumi::client {

using Bytes = std::vector<uint8_t>;

enum class ErrorCode {
  None,
  Closed,
  Timeout,
  Protocol,
  Transport,
  Backpressure,
  InvalidArgument
};

struct Error {
  ErrorCode code = ErrorCode::None;
  std::string message;
  explicit operator bool() const { return code != ErrorCode::None; }
};

struct Result {
  Bytes bytes;
  Error error;
};

enum class Protocol { Project, Agent, Host };

struct Options {
  // unix:/absolute/path, ws://host:port/rpc, or wss://host:port/rpc.
  std::string endpoint;
  Protocol protocol = Protocol::Project;
  std::string subprotocol;
  size_t max_frame_bytes = 16u << 20;
  size_t max_queued_bytes = 32u << 20;
  size_t max_pending_requests = 64;
  std::chrono::milliseconds connect_timeout{10000};
};

struct TransportEvents {
  std::function<void()> opened;
  std::function<void(Bytes)> received;
  std::function<void(Error)> closed;
  std::function<void()> tick;
};

// The adapter owns framing. Callbacks deliver complete protocol messages and
// must be serialized. Native adapters run them on their I/O thread; the browser
// adapter uses its event loop. Callbacks must not block waiting for another
// RPC. Send only enqueues data and must not invoke callbacks inline.
class Transport {
 public:
  virtual ~Transport() = default;
  virtual auto Start(TransportEvents events) -> void = 0;
  virtual auto Send(Bytes bytes) -> Error = 0;
  virtual auto Close() -> void = 0;
};

auto MakeTransport(const Options& options) -> std::shared_ptr<Transport>;

class Connection {
 public:
  using Completion = std::function<void(Result)>;
  using BuildRequest = std::function<Bytes(uint64_t)>;

  explicit Connection(Options options);
  Connection(Options options, std::shared_ptr<Transport> transport);
  ~Connection();
  Connection(const Connection&) = delete;
  auto operator=(const Connection&) -> Connection& = delete;

  // Start only opens the transport. The first call must be the protocol's
  // Hello.
  auto Start(std::function<void(Error)> completion) -> void;
  auto Start() -> std::future<Error>;
  auto OnEvent(std::function<void(Bytes)> callback) -> void;
  auto OnClosed(std::function<void(Error)> callback) -> void;

  // IDs are assigned under the connection lock and given to the encoder.
  auto Call(BuildRequest build, Completion completion,
            std::chrono::milliseconds timeout = std::chrono::seconds(60))
      -> uint64_t;
  auto Call(BuildRequest build,
            std::chrono::milliseconds timeout = std::chrono::seconds(60))
      -> std::future<Result>;

  // For generated-language bindings: frame must contain a nonzero request ID
  // greater than all IDs previously submitted on this connection.
  auto CallFrame(Bytes frame, Completion completion,
                 std::chrono::milliseconds timeout = std::chrono::seconds(60))
      -> void;
  auto CallFrame(Bytes frame,
                 std::chrono::milliseconds timeout = std::chrono::seconds(60))
      -> std::future<Result>;
  auto Close() -> void;

 private:
  struct State;
  std::shared_ptr<State> state_;
  std::shared_ptr<Transport> transport_;
};

}  // namespace tsugumi::client
