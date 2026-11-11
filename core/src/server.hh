// RPC transport: one poll loop owning every connection. Other threads talk to
// it only through post(); the inbox is processed in order, which is what keeps
// Open replies, DocPatched and run events correctly sequenced per peer.
#pragma once

#include <flatbuffers/flatbuffers.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "tsugumi/protocol/tsugumi_generated.h"

namespace tsugumi {

using ConnId = uint64_t;

struct Limits {
  uint32_t max_frame_bytes = 16u << 20;
  uint32_t max_connections = 128;
  uint32_t max_inflight = 64;
  uint32_t mutation_queue_limit = 128;
  uint32_t queue_wait_ms = 30000;
  uint32_t outbox_messages = 256;
  uint64_t outbox_bytes = 32ull << 20;
  uint32_t page_limit = 500;
  uint32_t file_chunk_bytes = 1u << 20;
  uint32_t log_chunk_bytes = 256u << 10;
  uint32_t hello_timeout_ms = 5000;
};

// A notification body built lazily per subscriber so that seq and
// subscription_id are stamped at actual send time.
struct Note {
  using Build = std::function<std::pair<Tsugumi::RPC::NotificationPayload, flatbuffers::Offset<void>>(
      flatbuffers::FlatBufferBuilder&)>;
  enum class Kind { Doc, Run, Display, Log };
  Kind kind = Kind::Doc;
  std::string coalesce_key;  // Display/Log: same key replaces an unsent note
  std::string card_key;      // Display: target card path key
  std::shared_ptr<const Build> build;
  size_t approx_bytes = 256;
};

struct RequestIn {
  ConnId conn = 0;
  uint64_t request_id = 0;
  std::shared_ptr<const std::string> frame;  // verified Message buffer
  const Tsugumi::RPC::Request* request() const;
  bool websocket = false;
};

class RequestSink {
 public:
  virtual ~RequestSink() = default;
  // Called on the loop thread for SessionHello; must be quick. Returns reply bytes
  // and whether the connection is now authenticated.
  virtual std::pair<std::string, bool> hello(const RequestIn& req) = 0;
  // Called on the loop thread for every other request after Hello.
  virtual void dispatch(RequestIn req) = 0;
  virtual void disconnected(ConnId conn) = 0;
};

struct ServerOptions {
  enum class Protocol { Project, Agent };
  Protocol protocol = Protocol::Project;
  std::string unix_path;           // empty: no Unix socket
  // WebSocket on a Unix socket (0600): the shared VM's projects, reached by
  // vmd only, so that nothing listens on the VM's loopback.
  std::string ws_unix_path;
  std::string tcp_host = "0.0.0.0";
  int tcp_port = -1;               // <0: no WebSocket listener
  std::vector<std::string> allowed_origins;
  // Launcher-owned control file lets another UI attach to a resident project.
  std::string origins_file;
  bool allow_missing_origin = false;
  Limits limits;
};

class Server {
 public:
  Server(ServerOptions opts, RequestSink* sink);
  ~Server();

  void start();  // binds listeners, spawns the loop thread
  void stop();
  int bound_tcp_port() const { return bound_port_; }

  // Thread safe commands. All are applied in posting order.
  void reply(ConnId conn, std::string bytes);
  void subscribe(ConnId conn, std::string sub_id, std::string open_reply);
  void unsubscribe(ConnId conn, std::string sub_id, std::string ack);
  void set_visible(ConnId conn, std::string sub_id, std::set<std::string> cards, bool enabled,
                   std::string ack, std::vector<Note> initial);
  void broadcast(Note note);
  void close(ConnId conn);
  size_t connection_count() const { return conn_count_.load(); }

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
  void enqueue_raw(Conn& c, std::string bytes, bool is_reply);
  void enqueue_note(Conn& c, const Note& n);
  void enforce_outbox(Conn& c);
  bool fill_write(Conn& c);
  void drop(Conn& c, const char* why);
  void wake();

  ServerOptions opts_;
  RequestSink* sink_;
  int unix_fd_ = -1;
  int ws_unix_fd_ = -1;
  int tcp_fd_ = -1;
  int wake_r_ = -1;
  int wake_w_ = -1;
  int bound_port_ = -1;
  std::thread thread_;
  std::atomic<bool> stopping_{false};
  std::atomic<size_t> conn_count_{0};
  std::mutex inbox_mu_;
  std::vector<Command> inbox_;
  std::map<ConnId, std::unique_ptr<Conn>> conns_;
  ConnId next_id_ = 1;
};

}  // namespace tsugumi
