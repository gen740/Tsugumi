#include "server.hh"

#include "tsugumi/workerd/value_wire.hh"

namespace tsugumi {
namespace R = Tsugumi::RPC;
namespace D = Tsugumi::Data;

namespace {

auto display_note(const CardPath& card, const std::string& run, uint64_t epoch,
                  std::string view, std::string bytes) -> Note {
  Note note;
  note.kind = Note::Kind::Display;
  note.card_key = path_key(card);
  note.coalesce_key = std::to_string(epoch) + "|" + view;
  note.approx_bytes = bytes.size() + view.size() + 1024;
  note.build = std::make_shared<const Note::Build>(
      [card, run, view = std::move(view), bytes = std::move(bytes)](auto& b) {
        const auto display = D::CreateDisplay(
            b, wire::path(b, card),
            run.empty() ? flatbuffers::Offset<flatbuffers::String>{}
                        : b.CreateString(run),
            b.CreateString(view),
            b.CreateVector(reinterpret_cast<const uint8_t*>(bytes.data()),
                           bytes.size()));
        return std::make_pair(R::NotificationPayload::DisplayUpdated,
                              R::CreateDisplayUpdated(b, display).Union());
      });
  return note;
}

auto display_run_note(const CardPath& card, const std::string& run,
                      D::DisplayRunState state) -> Note {
  Note note;
  note.kind = Note::Kind::DisplayRunState;
  note.card_key = path_key(card);
  note.build = std::make_shared<const Note::Build>([card, run, state](auto& b) {
    const auto value = R::CreateDisplayRunStateChanged(
        b, wire::path(b, card), b.CreateString(run), state);
    return std::make_pair(R::NotificationPayload::DisplayRunStateChanged,
                          value.Union());
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
    // A fresh key space for each Card after a control message or visibility
    // change. A newer frame cannot replace an older one across that boundary.
    std::map<std::string, uint64_t> display_intervals;
    uint64_t display_interval = 0;
  };
  RequestSink* sink;
  TransportServer transport;
  std::map<ConnId, std::shared_ptr<Subscription>> subscriptions;
  std::string display_run;
  uint64_t display_epoch = 0;
  std::map<std::string, CardPath> participating_cards;
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
    switch (note.kind) {
      case Note::Kind::CardViews:
      case Note::Kind::DisplayRunState:
        pending.trace = false;
        sub->display_intervals[note.card_key] = ++sub->display_interval;
        break;
      case Note::Kind::Display: {
        pending.trace = false;
        auto [interval, inserted] = sub->display_intervals.try_emplace(
            note.card_key, sub->display_interval + 1);
        if (inserted) ++sub->display_interval;
        pending.coalesce_key = "display|" + std::to_string(interval->second) +
                               "|" + note.coalesce_key;
        break;
      }
      default:
        pending.coalesce_key = std::move(note.coalesce_key);
        break;
    }
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
        state_->transport.send(conn, std::move(ack));
        if (!matches) return;
        const auto& sub = it->second;
        auto previous = std::move(sub->visible);
        const bool was_enabled = sub->enabled;
        sub->visible = std::move(cards);
        sub->enabled = enabled;
        std::erase_if(sub->display_intervals, [&](const auto& item) {
          return !enabled || !sub->visible.contains(item.first);
        });
        if (!enabled) return;
        for (auto& note : initial)
          if (sub->visible.contains(note.card_key) &&
              (!was_enabled || !previous.contains(note.card_key)))
            state_->send(conn, sub, std::move(note));
        for (const auto& card : sub->visible) {
          if (was_enabled && previous.contains(card)) continue;
          const auto active = state_->participating_cards.find(card);
          if (active != state_->participating_cards.end())
            state_->send(conn, sub,
                         display_run_note(active->second, state_->display_run,
                                          D::DisplayRunState::started));
        }
      });
}

void Server::broadcast(Note note) {
  state_->transport.ordered(
      [this, note = std::move(note)] { state_->broadcast(note); });
}

auto Server::display(const CardPath& card, std::string_view view,
                     std::string_view bytes) -> void {
  state_->transport.ordered([this, card, view = std::string(view),
                             bytes = std::string(bytes)]() mutable {
    state_->broadcast(display_note(card, state_->display_run,
                                   state_->display_epoch, std::move(view),
                                   std::move(bytes)));
  });
}

auto Server::begin_display_run(std::string run) -> void {
  state_->transport.ordered([this, run = std::move(run)] {
    state_->display_run = run;
    ++state_->display_epoch;
  });
}

auto Server::start_display_run(const CardPath& card, const std::string& run)
    -> void {
  state_->transport.ordered([this, card, run] {
    if (run != state_->display_run) return;
    if (state_->participating_cards.try_emplace(path_key(card), card).second)
      state_->broadcast(
          display_run_note(card, run, D::DisplayRunState::started));
  });
}

auto Server::end_display_run(const std::string& run) -> void {
  state_->transport.ordered([this, run] {
    if (run != state_->display_run) return;
    for (const auto& [key, card] : state_->participating_cards)
      state_->broadcast(display_run_note(card, run, D::DisplayRunState::ended));
    state_->participating_cards.clear();
    state_->display_run.clear();
    ++state_->display_epoch;
  });
}
}  // namespace tsugumi
