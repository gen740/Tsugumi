#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "tsugumi/rpc/client_transport.hh"

namespace tsugumi::client {

enum class Protocol { Project, Agent, Host };
struct Options : TransportOptions {
  Protocol protocol = Protocol::Project;
};

class Connection {
 public:
  using Completion = std::function<void(Result)>;
  // Builds a request for this connection using the supplied correlation ID.
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
            std::chrono::milliseconds timeout = {}) -> uint64_t;
  auto Call(BuildRequest build, std::chrono::milliseconds timeout = {})
      -> std::future<Result>;

  // For generated-language bindings: frame must contain a nonzero request ID
  // greater than all IDs previously submitted on this connection.
  auto CallFrame(Bytes frame, Completion completion,
                 std::chrono::milliseconds timeout = {}) -> void;
  auto Close() -> void;

 private:
  struct State;
  std::shared_ptr<State> state_;
  std::shared_ptr<Transport> transport_;
};

}  // namespace tsugumi::client
