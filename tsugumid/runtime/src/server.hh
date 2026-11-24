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
  enum class Kind { Doc, Run, Log, CardViews, Display };
  Kind kind = Kind::Doc;
  std::string coalesce_key;  // Same key replaces an unsent note
  std::string card_key;      // Target of a live view notification
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
  void set_visible(ConnId conn, std::string sub_id, std::set<std::string> cards,
                   bool enabled, std::string ack, std::vector<Note> initial);
  static auto notification(flatbuffers::FlatBufferBuilder& b,
                           std::string_view subscription, uint64_t seq,
                           Tsugumi::RPC::NotificationPayload type,
                           flatbuffers::Offset<void> value) -> std::string;
  void broadcast(Note note);
  auto display(const CardPath& card, std::string view, std::string bytes,
               std::function<void()> credit) -> void;
  auto return_view_credit(ConnId conn, const std::string& subscription,
                          const CardPath& card, const std::string& view,
                          uint64_t id, size_t bytes) -> void;
  void close(ConnId conn);

 private:
  struct State;
  std::unique_ptr<State> state_;
};
}  // namespace tsugumi
