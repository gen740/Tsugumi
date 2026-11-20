#include "server.hh"

namespace tsugumi {
namespace R = Tsugumi::RPC;

auto Server::notification(flatbuffers::FlatBufferBuilder& b,
                          std::string_view subscription, uint64_t seq,
                          R::NotificationPayload type,
                          flatbuffers::Offset<void> value) -> std::string {
  const auto id = b.CreateString(subscription.data(), subscription.size());
  const auto note = R::CreateNotification(b, id, seq, type, value);
  R::FinishMessageBuffer(
      b, R::CreateMessage(b, flatbuffers::nullopt, R::Payload::Notification,
                          note.Union()));
  return std::string(reinterpret_cast<const char*>(b.GetBufferPointer()),
                     b.GetSize());
}

struct Server::State : RequestSink {
  struct Subscription {
    std::string id;
    uint64_t seq = 1;
    std::set<std::string> visible;
    bool enabled = true;
  };
  RequestSink* sink;
  TransportServer transport;
  std::map<ConnId, std::shared_ptr<Subscription>> subscriptions;
  State(ServerOptions options, RequestSink* sink)
      : sink(sink), transport(std::move(options), this) {}
  auto hello(const RequestIn& in) -> std::pair<std::string, bool> override {
    return sink->hello(in);
  }
  void dispatch(RequestIn in) override { sink->dispatch(std::move(in)); }
  void disconnected(ConnId conn) override {
    subscriptions.erase(conn);
    sink->disconnected(conn);
  }
  void send(ConnId conn, const std::shared_ptr<Subscription>& sub, Note note) {
    PendingMessage pending;
    pending.coalesce_key = note.coalesce_key;
    pending.group = sub->id;
    pending.approx_bytes = note.approx_bytes;
    pending.build = [sub, build = std::move(note.build)] {
      flatbuffers::FlatBufferBuilder b;
      auto [type, value] = (*build)(b);
      return Server::notification(b, sub->id, ++sub->seq, type, value);
    };
    transport.send_note(conn, std::move(pending));
  }
};
Server::Server(ServerOptions options, RequestSink* sink)
    : state_(std::make_unique<State>(std::move(options), sink)) {}
Server::~Server() { stop(); }
void Server::start() { state_->transport.start(); }
void Server::stop() { state_->transport.stop(); }
int Server::bound_tcp_port() const {
  return state_->transport.bound_tcp_port();
}
void Server::reply(ConnId conn, std::string bytes) {
  state_->transport.reply(conn, std::move(bytes));
}
void Server::close(ConnId conn) { state_->transport.close(conn); }
void Server::subscribe(ConnId conn, std::string id, std::string reply) {
  state_->transport.ordered(conn, [this, conn, id = std::move(id),
                                   reply = std::move(reply)]() mutable {
    state_->transport.clear_notes(conn);
    auto sub = std::make_shared<State::Subscription>();
    sub->id = std::move(id);
    state_->subscriptions[conn] = std::move(sub);
    state_->transport.send(conn, std::move(reply));
  });
}
void Server::set_visible(ConnId conn, std::string id,
                         std::set<std::string> cards, bool enabled,
                         std::string ack, std::vector<Note> initial) {
  state_->transport.ordered(
      conn, [this, conn, id = std::move(id), cards = std::move(cards), enabled,
             ack = std::move(ack), initial = std::move(initial)]() mutable {
        auto it = state_->subscriptions.find(conn);
        const bool matches =
            it != state_->subscriptions.end() && it->second->id == id;
        if (matches) {
          it->second->visible = std::move(cards);
          it->second->enabled = enabled;
        }
        state_->transport.send(conn, std::move(ack));
        if (matches && enabled)
          for (auto& note : initial)
            if (it->second->visible.count(note.card_key))
              state_->send(conn, it->second, std::move(note));
      });
}
void Server::broadcast(Note note) {
  state_->transport.ordered([this, note = std::move(note)] {
    for (const auto& [conn, sub] : state_->subscriptions) {
      if (sub->id.empty()) continue;
      if (note.kind == Note::Kind::Display &&
          (!sub->enabled || !sub->visible.count(note.card_key)))
        continue;
      state_->send(conn, sub, note);
    }
  });
}
}  // namespace tsugumi
