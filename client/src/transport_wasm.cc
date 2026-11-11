#include <emscripten/eventloop.h>
#include <emscripten/websocket.h>

#include <stdexcept>
#include <utility>

#include "tsugumi/client/client.hh"

namespace tsugumi::client {
namespace {

class BrowserTransport final : public Transport {
  struct State : std::enable_shared_from_this<State> {
    explicit State(Options value) : options(std::move(value)) {}
    Options options;
    TransportEvents events;
    EMSCRIPTEN_WEBSOCKET_T socket = 0;
    int interval = 0;
    bool opened = false;
    bool closed = false;
    std::chrono::steady_clock::time_point deadline;

    auto End(Error error) -> void {
      if (closed) return;
      closed = true;
      opened = false;
      if (interval) {
        emscripten_clear_interval(interval);
        interval = 0;
      }
      if (socket > 0) {
        emscripten_websocket_set_onopen_callback(socket, nullptr, nullptr);
        emscripten_websocket_set_onmessage_callback(socket, nullptr, nullptr);
        emscripten_websocket_set_onerror_callback(socket, nullptr, nullptr);
        emscripten_websocket_set_onclose_callback(socket, nullptr, nullptr);
        emscripten_websocket_close(socket, 1000, "");
        emscripten_websocket_delete(socket);
        socket = 0;
      }
      if (events.closed) events.closed(std::move(error));
    }
  };

 public:
  explicit BrowserTransport(Options options)
      : state_(std::make_shared<State>(std::move(options))) {}
  ~BrowserTransport() override { Close(); }

  auto Start(TransportEvents events) -> void override {
    auto state = state_;
    if (state->socket || state->closed)
      throw std::runtime_error("transport already started");
    if (!emscripten_websocket_is_supported())
      throw std::runtime_error("WebSocket is unavailable");
    state->events = std::move(events);
    const auto protocol = state->options.subprotocol.empty()
                              ? state->options.protocol == Protocol::Agent
                                    ? "tsugumi.agent.v1"
                                    : "tsugumi.rpc.v1"
                              : state->options.subprotocol;
    EmscriptenWebSocketCreateAttributes attributes{};
    attributes.url = state->options.endpoint.c_str();
    attributes.protocols = protocol.c_str();
    attributes.createOnMainThread = false;
    state->socket = emscripten_websocket_new(&attributes);
    if (state->socket <= 0)
      throw std::runtime_error("cannot create browser WebSocket");
    emscripten_websocket_set_onopen_callback(
        state->socket, state.get(),
        +[](int, const EmscriptenWebSocketOpenEvent*, void* context) {
          auto current = static_cast<State*>(context)->shared_from_this();
          if (!current->closed) {
            current->opened = true;
            if (current->events.opened) current->events.opened();
          }
          return true;
        });
    emscripten_websocket_set_onmessage_callback(
        state->socket, state.get(),
        +[](int, const EmscriptenWebSocketMessageEvent* event, void* context) {
          auto current = static_cast<State*>(context)->shared_from_this();
          if (current->closed) return true;
          if (event->isText || !event->numBytes ||
              event->numBytes > current->options.max_frame_bytes) {
            current->End(
                {ErrorCode::Protocol, "invalid WebSocket RPC message"});
          } else if (current->events.received) {
            current->events.received(
                Bytes(event->data, event->data + event->numBytes));
          }
          return true;
        });
    emscripten_websocket_set_onerror_callback(
        state->socket, state.get(),
        +[](int, const EmscriptenWebSocketErrorEvent*, void* context) {
          auto current = static_cast<State*>(context)->shared_from_this();
          current->End({ErrorCode::Transport, "WebSocket connection failed"});
          return true;
        });
    emscripten_websocket_set_onclose_callback(
        state->socket, state.get(),
        +[](int, const EmscriptenWebSocketCloseEvent*, void* context) {
          auto current = static_cast<State*>(context)->shared_from_this();
          current->End({ErrorCode::Closed, "WebSocket connection closed"});
          return true;
        });
    state->deadline =
        std::chrono::steady_clock::now() + state->options.connect_timeout;
    state->interval = emscripten_set_interval(
        +[](void* context) {
          auto current = static_cast<State*>(context)->shared_from_this();
          if (!current->opened &&
              std::chrono::steady_clock::now() >= current->deadline)
            current->End(
                {ErrorCode::Timeout, "WebSocket connection timed out"});
          else if (!current->closed && current->events.tick)
            current->events.tick();
        },
        20, state.get());
  }

  auto Send(Bytes bytes) -> Error override {
    if (!state_->opened || state_->closed)
      return {ErrorCode::Closed, "WebSocket is not open"};
    if (bytes.empty() || bytes.size() > state_->options.max_frame_bytes)
      return {ErrorCode::InvalidArgument, "RPC message exceeds its limit"};
    size_t buffered = 0;
    if (emscripten_websocket_get_buffered_amount(state_->socket, &buffered) !=
        EMSCRIPTEN_RESULT_SUCCESS)
      return {ErrorCode::Transport, "cannot inspect WebSocket outbox"};
    if (buffered + bytes.size() > state_->options.max_queued_bytes)
      return {ErrorCode::Backpressure, "WebSocket outbox is full"};
    if (emscripten_websocket_send_binary(state_->socket, bytes.data(),
                                         static_cast<uint32_t>(bytes.size())) !=
        EMSCRIPTEN_RESULT_SUCCESS)
      return {ErrorCode::Transport, "WebSocket send failed"};
    return {};
  }

  auto Close() -> void override {
    auto state = state_;
    state->End({ErrorCode::Closed, "RPC connection closed"});
  }

 private:
  std::shared_ptr<State> state_;
};

}  // namespace

auto MakeTransport(const Options& options) -> std::shared_ptr<Transport> {
  if (!options.endpoint.starts_with("ws://") &&
      !options.endpoint.starts_with("wss://"))
    throw std::invalid_argument("browser RPC requires a WebSocket endpoint");
  if (!options.max_frame_bytes || options.max_frame_bytes > UINT32_MAX ||
      options.max_queued_bytes < options.max_frame_bytes ||
      options.connect_timeout.count() <= 0)
    throw std::invalid_argument("invalid RPC transport limits");
  return std::make_shared<BrowserTransport>(options);
}

}  // namespace tsugumi::client
