#include "server.hh"

#include <algorithm>
#include <condition_variable>

#include "tsugumi/workerd/frame.hh"
#include "tsugumi/workerd/value_wire.hh"

namespace tsugumi {
namespace R = Tsugumi::RPC;
namespace D = Tsugumi::Data;

namespace {

auto display_note(const CardPath& card, std::string view, std::string bytes,
                  uint64_t credit) -> Note {
  Note note;
  note.kind = Note::Kind::Display;
  note.card_key = path_key(card);
  note.approx_bytes = bytes.size() + view.size() + 1024;
  note.build = std::make_shared<const Note::Build>(
      [card, view = std::move(view), bytes = std::move(bytes), credit](auto& b) {
        const auto display = D::CreateDisplay(
            b, wire::path(b, card),
            b.CreateString(view),
            b.CreateVector(reinterpret_cast<const uint8_t*>(bytes.data()),
                           bytes.size()), credit);
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
    std::set<std::string> visible;
    bool enabled = true;
  };
  RequestSink* sink;
  TransportServer transport;
  std::map<ConnId, std::shared_ptr<Subscription>> subscriptions;
  struct Credit {
    size_t used = 0;
    std::map<uint64_t, size_t> messages;
  };
  struct ViewTarget {
    std::string id;
    std::set<std::string> cards;
    std::map<std::pair<std::string, std::string>, Credit> credits;
  };
  std::mutex relay_mutex;
  std::condition_variable_any relay_ready;
  std::map<ConnId, ViewTarget> targets;
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
      targets.erase(conn);
    }
    relay_ready.notify_all();
    sink->disconnected(conn);
  }
  void send(ConnId conn, const std::shared_ptr<Subscription>& sub, Note note) {
    PendingMessage pending;
    pending.trace = note.kind != Note::Kind::CardViews &&
                    note.kind != Note::Kind::Display;
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
      if (!note.card_key.empty() &&
          (!sub->enabled || !sub->visible.contains(note.card_key)))
        continue;
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
    state_->targets.clear();
  }
  state_->relay_ready.notify_all();
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
    {
      std::lock_guard lock(state_->relay_mutex);
      state_->targets[conn] = State::ViewTarget{sub->id, {}, {}};
    }
    state_->relay_ready.notify_all();
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
        state_->transport.send(conn, std::move(ack));
        if (!matches) return;
        const auto& sub = it->second;
        auto previous = std::move(sub->visible);
        const bool was_enabled = sub->enabled;
        sub->visible = std::move(cards);
        sub->enabled = enabled;
        {
          std::lock_guard lock(state_->relay_mutex);
          auto& target = state_->targets.at(conn);
          target.cards = enabled ? sub->visible : std::set<std::string>{};
          std::erase_if(target.credits, [&](const auto& entry) {
            return !target.cards.contains(entry.first.first);
          });
        }
        state_->relay_ready.notify_all();
        if (!enabled) return;
        for (auto& note : initial)
          if (sub->visible.contains(note.card_key) &&
              (!was_enabled || !previous.contains(note.card_key)))
            state_->send(conn, sub, std::move(note));
      });
}

void Server::broadcast(Note note) {
  state_->transport.ordered(
      [this, note = std::move(note)] { state_->broadcast(note); });
}

auto Server::display(const CardPath& card, std::string_view view,
                     std::string_view bytes, std::stop_token stop) -> void {
  if (bytes.size() > TSUGUMI_MESSAGE_MAX || view.empty()) return;
  const auto key = std::make_pair(path_key(card), std::string(view));
  ConnId conn = 0;
  std::string subscription;
  uint64_t id = 0;
  {
    std::unique_lock lock(state_->relay_mutex);
    const auto listener = [&] {
      return std::ranges::find_if(state_->targets, [&](const auto& item) {
        return item.second.cards.contains(key.first);
      });
    };
    if (!state_->relay_ready.wait(lock, stop, [&] {
          const auto target = listener();
          if (target == state_->targets.end()) return true;
          const auto credit = target->second.credits.find(key);
          return credit == target->second.credits.end() ||
                 credit->second.used <= workerd::kViewBudget - TSUGUMI_MESSAGE_MAX;
        }) || stop.stop_requested())
      return;
    const auto target = listener();
    if (target == state_->targets.end()) return;  // no receiver
    conn = target->first;
    subscription = target->second.id;
    auto& credit = target->second.credits[key];
    const auto charge = workerd::message_cost(bytes.size());
    id = ++state_->next_credit;
    credit.used += charge;
    credit.messages.emplace(id, charge);
    // Order posting with admission, even when several invocations send to one view.
    state_->transport.ordered(conn, [this, conn, subscription, key, id,
                                     note = display_note(card, key.second,
                                                         std::string(bytes), id)] {
      const auto target = state_->subscriptions.find(conn);
      if (target == state_->subscriptions.end() || target->second->id != subscription ||
          !target->second->visible.contains(key.first))
        return;
      {
        std::lock_guard check(state_->relay_mutex);
        const auto& credits = state_->targets.at(conn).credits;
        const auto slot = credits.find(key);
        if (slot == credits.end() || !slot->second.messages.contains(id)) return;
      }
      state_->send(conn, target->second, note);
    });
  }
}

auto Server::return_view_credit(ConnId conn, const std::string& subscription,
                                 const CardPath& card, const std::string& view,
                                 uint64_t id, size_t bytes) -> void {
  std::lock_guard lock(state_->relay_mutex);
  const auto target = state_->targets.find(conn);
  if (target == state_->targets.end() || target->second.id != subscription) return;
  const auto credit = target->second.credits.find({path_key(card), view});
  if (credit == target->second.credits.end()) return;
  const auto message = credit->second.messages.find(id);
  if (message == credit->second.messages.end() ||
      message->second != workerd::message_cost(bytes))
    return;
  credit->second.used -= message->second;
  credit->second.messages.erase(message);
  if (credit->second.messages.empty()) target->second.credits.erase(credit);
  state_->relay_ready.notify_all();
}
}  // namespace tsugumi
