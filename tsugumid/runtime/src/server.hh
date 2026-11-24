// Project notification endpoint over the protocol-free transport loop.
#pragma once
#include "tsugumi/document/document.hh"
#include "tsugumi/protocol/tsugumi_generated.h"
#include "tsugumi/rpc/transport.hh"
namespace tsugumi {
inline auto project_request(const RequestIn& in)
    -> const Tsugumi::RPC::Request* {
  return Tsugumi::RPC::GetMessage(in.frame->data())->payload_as_Request();
}
auto ProjectEndpoint(ServerOptions& options) -> void;
// A notification body built lazily per subscriber so that seq and
// subscription_id are stamped at actual send time.
struct Note {
  using Build = std::function<
      std::pair<Tsugumi::RPC::NotificationPayload, flatbuffers::Offset<void>>(
          flatbuffers::FlatBufferBuilder&)>;
  enum class Kind { Doc, Run, Log, ViewControl, Display };
  Kind kind = Kind::Doc;
  std::string coalesce_key;  // Same key replaces an unsent note
  std::shared_ptr<const Build> build;
  size_t approx_bytes = 256;
};

class Server {
 public:
  Server(ServerOptions opts, RequestSink* sink);
  ~Server();

  void start();  // binds listeners, spawns the loop thread
  void stop();
  int bound_tcp_port() const;

  // Thread safe commands. All are applied in posting order.
  void reply(ConnId conn, std::string bytes);
  void subscribe(ConnId conn, std::string sub_id, std::string open_reply);
  auto open_view(ConnId conn, const std::string& subscription,
                 const std::string& session, Note note) -> void;
  auto view_note(ConnId conn, const std::string& subscription, Note note)
      -> void;
  auto close_view(ConnId conn, const std::string& session) -> void;
  static auto notification(flatbuffers::FlatBufferBuilder& b,
                           std::string_view subscription, uint64_t seq,
                           Tsugumi::RPC::NotificationPayload type,
                           flatbuffers::Offset<void> value) -> std::string;
  void broadcast(Note note);
  auto display(ConnId conn, const std::string& session, std::string bytes,
               std::function<void()> credit) -> void;
  auto return_view_credit(ConnId conn, const std::string& session, uint64_t id,
                          size_t bytes) -> void;
  void close(ConnId conn);

 private:
  struct State;
  std::unique_ptr<State> state_;
};
}  // namespace tsugumi
