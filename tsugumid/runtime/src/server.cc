#include "server.hh"

#include <algorithm>
#include <deque>

#include "tsugumi/workerd/frame.hh"

namespace tsugumi {
namespace R = Tsugumi::RPC;
namespace D = Tsugumi::Data;

namespace {

auto display_note(std::string session, std::string bytes, uint64_t credit)
    -> Note {
  Note note;
  note.kind = Note::Kind::Display;
  note.approx_bytes = bytes.size() + session.size() + 1024;
  note.build = std::make_shared<const Note::Build>(
      [session = std::move(session), bytes = std::move(bytes),
       credit](auto& b) {
        const auto display = D::CreateDisplay(
            b, b.CreateString(session),
            b.CreateVector(reinterpret_cast<const uint8_t*>(bytes.data()),
                           bytes.size()),
            credit);
        return std::make_pair(R::NotificationPayload::DisplayUpdated,
                              R::CreateDisplayUpdated(b, display).Union());
      });
  return note;
}

}  // namespace

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
  };
  RequestSink* sink;
  TransportServer transport;
  std::map<ConnId, std::shared_ptr<Subscription>> subscriptions;
  using ViewKey = std::pair<ConnId, std::string>;
  struct Display {
    std::string bytes;
    std::function<void()> complete;
  };
  struct Credit {
    std::string subscription;
    size_t used = 0;
    std::map<uint64_t, size_t> messages;
    size_t queued = 0;
    std::deque<Display> pending;
    // These messages still occupy the sending worker's credit window.
    std::deque<Display> ingress;
  };
  std::mutex relay_mutex;
  std::map<ViewKey, Credit> credits;
  uint64_t next_credit = 0;
  State(ServerOptions options, RequestSink* sink)
      : sink(sink), transport(std::move(options), this) {}
  auto hello(const RequestIn& in) -> std::pair<std::string, bool> override {
    return sink->hello(in);
  }
  void dispatch(RequestIn in) override { sink->dispatch(std::move(in)); }
  void disconnected(ConnId conn) override {
    subscriptions.erase(conn);
    {
      std::lock_guard lock(relay_mutex);
      std::erase_if(credits, [&](const auto& entry) {
        if (entry.first.first != conn) return false;
        discard(entry.second);
        return true;
      });
    }
    sink->disconnected(conn);
  }
  static auto discard(const Credit& credit) -> void {
    for (auto& item : credit.ingress)
      if (item.complete) item.complete();
  }

  // Called with relay_mutex held. A stalled client consumes just this view's
  // windows. The worker socket continues to drain other views and results.
  auto flush(const ViewKey& key, Credit& credit) -> void {
    const auto conn = key.first;
    constexpr auto threshold = workerd::kViewBudget - TSUGUMI_MESSAGE_MAX;
    for (;;) {
      while (credit.queued <= threshold && !credit.ingress.empty()) {
        auto item = std::move(credit.ingress.front());
        credit.ingress.pop_front();
        credit.queued += workerd::message_cost(item.bytes.size());
        if (auto complete = std::exchange(item.complete, {})) complete();
        credit.pending.push_back(std::move(item));
      }
      if (credit.used > threshold || credit.pending.empty()) return;
      auto item = std::move(credit.pending.front());
      credit.pending.pop_front();
      const auto charge = workerd::message_cost(item.bytes.size());
      credit.queued -= charge;
      credit.used += charge;
      const auto id = ++next_credit;
      credit.messages.emplace(id, charge);
      transport.ordered(
          conn, [this, conn, subscription = credit.subscription, key, id,
                 note = display_note(key.second, std::move(item.bytes), id)] {
            const auto sub = subscriptions.find(conn);
            if (sub == subscriptions.end() || sub->second->id != subscription)
              return;
            {
              std::lock_guard lock(relay_mutex);
              const auto slot = credits.find(key);
              if (slot == credits.end() || !slot->second.messages.contains(id))
                return;
            }
            send(conn, sub->second, note);
          });
    }
  }

  void send(ConnId conn, const std::shared_ptr<Subscription>& sub, Note note) {
    PendingMessage pending;
    pending.trace =
        note.kind != Note::Kind::ViewControl && note.kind != Note::Kind::Display;
    pending.coalesce_key = std::move(note.coalesce_key);
    pending.group = sub->id;
    pending.approx_bytes = note.approx_bytes;
    pending.build = [sub, build = std::move(note.build)] {
      flatbuffers::FlatBufferBuilder b;
      auto [type, value] = (*build)(b);
      return Server::notification(b, sub->id, ++sub->seq, type, value);
    };
    transport.send_note(conn, std::move(pending));
  }

  auto broadcast(const Note& note) -> void {
    for (const auto& [conn, sub] : subscriptions) {
      if (sub->id.empty()) continue;
      send(conn, sub, note);
    }
  }
};
Server::Server(ServerOptions options, RequestSink* sink)
    : state_(std::make_unique<State>(std::move(options), sink)) {}
Server::~Server() { stop(); }
void Server::start() { state_->transport.start(); }
void Server::stop() {
  state_->transport.stop();
  {
    std::lock_guard lock(state_->relay_mutex);
    for (const auto& [key, credit] : state_->credits) State::discard(credit);
    state_->credits.clear();
  }
}
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
auto Server::open_view(ConnId conn, const std::string& subscription,
                       const std::string& session, Note note) -> void {
  std::lock_guard lock(state_->relay_mutex);
  state_->credits[{conn, session}].subscription = subscription;
  view_note(conn, subscription, std::move(note));
}

auto Server::view_note(ConnId conn, const std::string& subscription, Note note)
    -> void {
  state_->transport.ordered(conn, [this, conn, subscription,
                                   note = std::move(note)] {
    const auto sub = state_->subscriptions.find(conn);
    if (sub != state_->subscriptions.end() && sub->second->id == subscription)
      state_->send(conn, sub->second, note);
  });
}

auto Server::close_view(ConnId conn, const std::string& session) -> void {
  std::lock_guard lock(state_->relay_mutex);
  const auto found = state_->credits.find({conn, session});
  if (found == state_->credits.end()) return;
  State::discard(found->second);
  state_->credits.erase(found);
}

void Server::broadcast(Note note) {
  state_->transport.ordered(
      [this, note = std::move(note)] { state_->broadcast(note); });
}

auto Server::display(ConnId conn, const std::string& session, std::string bytes,
                     std::function<void()> complete) -> void {
  std::lock_guard lock(state_->relay_mutex);
  const auto found = state_->credits.find({conn, session});
  if (found == state_->credits.end() || bytes.size() > TSUGUMI_MESSAGE_MAX) {
    if (complete) complete();
    return;
  }
  auto& credit = found->second;
  credit.ingress.push_back({std::move(bytes), std::move(complete)});
  state_->flush(found->first, credit);
}

auto Server::return_view_credit(ConnId conn, const std::string& session,
                                uint64_t id, size_t bytes) -> void {
  std::lock_guard lock(state_->relay_mutex);
  const auto found = state_->credits.find({conn, session});
  if (found == state_->credits.end()) return;
  auto& credit = found->second;
  const auto message = credit.messages.find(id);
  if (message == credit.messages.end() ||
      message->second != workerd::message_cost(bytes))
    return;
  credit.used -= message->second;
  credit.messages.erase(message);
  state_->flush(found->first, credit);
}
}  // namespace tsugumi
