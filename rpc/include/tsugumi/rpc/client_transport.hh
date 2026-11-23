// Protocol-free native/browser transport interface.
#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
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

struct TransportOptions {
  // unix:/absolute/path, ws://host:port/rpc, or wss://host:port/rpc.
  std::string endpoint;
  std::string subprotocol;
  size_t max_frame_bytes = (16u << 20) + 65536;
};

struct TransportEvents {
  std::function<void()> opened;
  std::function<void(Bytes)> received;
  std::function<void(Error)> closed;
  std::function<void()> tick;
  std::function<std::optional<std::chrono::steady_clock::time_point>()>
      deadline;
};

// The adapter owns framing and enforces the configured frame size on send
// and receive. Callbacks deliver complete protocol messages and
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

auto MakeTransport(const TransportOptions& options)
    -> std::shared_ptr<Transport>;

}  // namespace tsugumi::client
