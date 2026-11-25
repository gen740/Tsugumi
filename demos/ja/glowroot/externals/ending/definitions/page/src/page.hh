// Glowroot's page helpers, copied into every page Card: the scene sent to
// the "screen" view and the events read back from it.
#pragma once

#include "game.hh"

namespace game {

// Colours are 0xRRGGBBAA.
inline constexpr uint32_t kText = 0xf4efe0ff;
inline constexpr uint32_t kDim = 0xb9c4b0ff;
inline constexpr uint32_t kGold = 0xffd77aff;
inline constexpr uint32_t kGreen = 0x9fe08aff;

struct Scene {
  glow::screen::SceneT value;

  explicit Scene(std::string screen) { value.screen = std::move(screen); }

  auto label(std::string text, float x, float y, float size = 24, uint32_t color = kText,
             uint8_t align = 0) -> glow::screen::LabelT& {
    auto& item = *value.text.emplace_back(std::make_unique<glow::screen::LabelT>());
    item.text = std::move(text);
    item.x = x;
    item.y = y;
    item.size = size;
    item.color = color;
    item.align = align;
    return item;
  }
  auto counter(std::string text, double target, float x, float y, float size = 24,
               uint32_t color = kText, uint8_t align = 0) -> glow::screen::LabelT& {
    auto& item = label(std::move(text), x, y, size, color, align);
    item.value = target;
    item.counter = true;
    return item;
  }
  auto button(std::string id, std::string label, std::string detail, float x, float y, float w,
              float h, bool enabled = true, uint8_t style = 0) -> glow::screen::ButtonT& {
    auto& item = *value.buttons.emplace_back(std::make_unique<glow::screen::ButtonT>());
    item.id = std::move(id);
    item.label = std::move(label);
    item.detail = std::move(detail);
    item.x = x;
    item.y = y;
    item.w = w;
    item.h = h;
    item.enabled = enabled;
    item.style = style;
    return item;
  }
  auto node(std::string id, std::string label, std::string detail, float x, float y,
            uint8_t state, int16_t parent, uint32_t color) -> glow::screen::NodeT& {
    auto& item = *value.tree.emplace_back(std::make_unique<glow::screen::NodeT>());
    item.id = std::move(id);
    item.label = std::move(label);
    item.detail = std::move(detail);
    item.x = x;
    item.y = y;
    item.state = state;
    item.parent = parent;
    item.color = color;
    return item;
  }
  auto fx(uint8_t kind, float x, float y) -> void {
    auto& item = *value.fx.emplace_back(std::make_unique<glow::screen::FxT>());
    item.kind = kind;
    item.x = x;
    item.y = y;
  }
};

struct Event {
  enum Kind { kNone, kButton, kClosed, kCancelled } kind = kNone;
  std::string button;
};

// One page: a route of this process for the window, the scenes it sends and
// the events it reads. The route is dropped when the page leaves; the window
// stays for the next page.
class Page {
 public:
  explicit Page(const tsugumi_view_session& session)
      : session_(session), route_(card::display::route_register(session)) {}
  Page(const tsugumi_view_session& session, const tsugumi_route& route)
      : session_(session), route_(route) {}
  Page(const Page&) = delete;
  auto operator=(const Page&) -> Page& = delete;
  ~Page() {
    try {
      leave();
    } catch (...) {
      // The process ends soon after; its routes go with it.
    }
  }

  auto leave() -> void {
    if (!left_) card::display::route_unregister(route_);
    left_ = true;
  }

  auto show(Scene& scene) -> void {
    scene.value.reply = std::make_unique<glow::screen::Route>(
        flatbuffers::span<const uint8_t, 16>(route_.bytes));
    flatbuffers::FlatBufferBuilder builder(4096);
    builder.Finish(glow::screen::Scene::Pack(builder, &scene.value));
    // A full queue to the window only delays the next frame; a closed window
    // drops the message and the close event ends the page.
    card::display::send_raw(session_, {builder.GetBufferPointer(), builder.GetSize()});
  }

  // Waits at most timeout_ms for the player. Time itself is measured by the
  // caller with a monotonic clock; the timeout only bounds the wait.
  auto next(int64_t timeout_ms = 100) -> Event {
    const auto received = card::display::recv(timeout_ms);
    using card::display::ReceiveStatus;
    switch (received.status) {
      case ReceiveStatus::kCancelled:
        return {Event::kCancelled, {}};
      case ReceiveStatus::kClose:
        left_ = true;  // Closing the session also removes the route.
        return {Event::kClosed, {}};
      case ReceiveStatus::kTimeout:
        return {};
      case ReceiveStatus::kMessage:
        break;
    }
    flatbuffers::Verifier verifier(received.bytes.data(), received.bytes.size());
    if (!verifier.VerifyBuffer<glow::screen::Event>(nullptr)) return {};
    const auto* event = flatbuffers::GetRoot<glow::screen::Event>(received.bytes.data());
    if (!event->button()) return {};
    return {Event::kButton, event->button()->str()};
  }

 private:
  tsugumi_view_session session_;
  tsugumi_route route_;
  bool left_ = false;
};

// Milliseconds between calls, from the language's monotonic clock.
class Stopwatch {
 public:
  auto lap() -> double {
    const auto now = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(now - last_).count();
    last_ = now;
    return ms;
  }

 private:
  std::chrono::steady_clock::time_point last_ = std::chrono::steady_clock::now();
};

}  // namespace game
