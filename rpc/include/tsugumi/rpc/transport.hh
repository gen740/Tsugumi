// RPC transport: one poll loop owning every connection. Other threads talk to
// it only through post(); the inbox is processed in order, which is what keeps
// Open replies, DocPatched and run events correctly sequenced per peer.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "tsugumi/rpc/trace.hh"
#include "tsugumi/rpc/unix_binding.hh"

namespace tsugumi {

using ConnId = uint64_t;

struct TransportLimits {
  uint32_t max_frame_bytes = 16u << 20;
};

struct PendingMessage {
  std::string coalesce_key;
  std::string group;
  std::function<std::string()> build;
  size_t approx_bytes = 256;
  bool trace = true;
};
struct FrameInfo {
  uint64_t request_id;
  bool hello;
  const char* peer = nullptr;
  bool trace = true;
};
enum class AdmissionFailure { AlreadyHello, Unauthenticated, Busy };
struct FrameCodec {
  std::function<std::optional<FrameInfo>(std::string_view, uint32_t)> inspect;
  std::function<uint64_t(std::string_view)> completed;
  std::function<std::string(uint64_t, AdmissionFailure)> failure;
};

struct RequestIn {
  ConnId conn = 0;
  uint64_t request_id = 0;
  std::shared_ptr<const std::string> frame;  // verified Message buffer
  bool websocket = false;
  std::shared_ptr<std::atomic<bool>> cancelled = {};
  std::stop_token connection_stop = {};
};

class RequestSink {
 public:
  virtual ~RequestSink() = default;
  // Called on the loop thread for SessionHello; must be quick. Returns reply
  // bytes and whether the connection is now authenticated.
  virtual std::pair<std::string, bool> hello(const RequestIn& req) = 0;
  // Called on the loop thread for every other request after Hello.
  virtual void dispatch(RequestIn req) = 0;
  virtual void disconnected(ConnId conn) = 0;
};

struct ServerOptions {
  FrameCodec codec;
  const char* trace_endpoint = "";
  const trace::Codec* trace_codec = nullptr;
  std::string subprotocol;
  std::string unix_path;  // empty: no Unix socket
  // WebSocket on a Unix socket (0600): the shared VM's projects, reached by
  // vmd only, so that nothing listens on the VM's loopback.
  std::string ws_unix_path;
  std::string tcp_host = "0.0.0.0";
  int tcp_port = -1;  // <0: no WebSocket listener
  std::vector<std::string> allowed_origins;
  // Origins the launcher allows later, so another UI can attach to a resident
  // project: asked for an origin outside allowed_origins.
  std::function<bool(const std::string& origin)> origin_allowed;
  bool allow_missing_origin = false;
  TransportLimits limits;
};

class TransportServer {
 public:
  TransportServer(ServerOptions opts, RequestSink* sink);
  ~TransportServer();

  void start();  // binds listeners, spawns the loop thread
  void stop();
  int bound_tcp_port() const { return bound_port_; }

  // Thread safe commands. All are applied in posting order.
  void reply(ConnId conn, std::string bytes);
  // Endpoint state changes and sends run on the same ordered loop.
  void ordered(std::function<void()> action);
  void ordered(ConnId conn, std::function<void()> action);
  // Takes a connected stream socket whose WebSocket handshake (`head`) was
  // read and checked by another listener (the Web edge).
  void adopt(int fd, std::string head);
  // The following methods are called only by ordered actions.
  void send(ConnId conn, std::string bytes);
  void send_note(ConnId conn, PendingMessage note);
  void clear_notes(ConnId conn);
  void close(ConnId conn);

 private:
  struct Conn;
  struct Command;
  void loop();
  void post(Command c);
  void apply(Command& c);
  void accept_on(int fd, bool websocket);
  void on_readable(Conn& c);
  void on_writable(Conn& c);
  void handle_frame(Conn& c, std::string frame);
  bool handle_ws_handshake(Conn& c);
  void parse_ws(Conn& c);
  void enqueue_raw(Conn& c, std::string bytes, bool is_reply,
                   bool trace = true);
  void enqueue_note(Conn& c, const PendingMessage& n);
  bool fill_write(Conn& c);
  void drop(Conn& c, const char* why);
  void wake();

  ServerOptions opts_;
  RequestSink* sink_;
  int unix_fd_ = -1;
  int ws_unix_fd_ = -1;
  unix_socket::Binding unix_binding_, ws_unix_binding_;
  int tcp_fd_ = -1;
  int wake_r_ = -1;
  int wake_w_ = -1;
  int bound_port_ = -1;
  std::thread thread_;
  std::atomic<bool> stopping_{false};
  std::mutex inbox_mu_;
  std::vector<Command> inbox_;
  std::map<ConnId, std::unique_ptr<Conn>> conns_;
  ConnId next_id_ = 1;
};

}  // namespace tsugumi
