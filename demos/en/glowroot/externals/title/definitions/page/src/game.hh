// Glowroot's state helpers, copied into every Card that needs them.
#pragma once

#include "card.hh"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace game {

// An owned copy of an input GameState. The input types live in their own
// namespaces (one per port), so this reads them by their field names.
template <class In>
auto own_state(const In* in) -> std::unique_ptr<glow::GameStateT> {
  auto out = std::make_unique<glow::GameStateT>();
  out->boosts = std::make_unique<glow::BoostsT>();
  if (!in) return out;
  out->light = in->light();
  out->lifetime = in->lifetime();
  out->total = in->total();
  if (in->roots()) out->roots.assign(in->roots()->begin(), in->roots()->end());
  out->seeds = in->seeds();
  if (in->nodes()) out->nodes.assign(in->nodes()->begin(), in->nodes()->end());
  out->replants = in->replants();
  out->elapsed_ms = in->elapsed_ms();
  out->clicks = in->clicks();
  if (const auto* boosts = in->boosts()) {
    out->boosts->click = boosts->click();
    out->boosts->roots = boosts->roots();
    out->boosts->seeds = boosts->seeds();
  }
  out->bloom = in->bloom();
  return out;
}

inline auto copy_state(const glow::GameStateT& in) -> std::unique_ptr<glow::GameStateT> {
  auto out = std::make_unique<glow::GameStateT>(in);
  return out;
}

template <class In>
auto own_view(const In* in) -> std::unique_ptr<glow::ViewT> {
  std::array<uint8_t, 16> bytes{};
  if (in && in->session())
    for (uint32_t i = 0; i < 16; ++i) bytes[i] = in->session()->bytes()->Get(i);
  auto out = std::make_unique<glow::ViewT>();
  out->session = std::make_unique<glow::ViewSession>(flatbuffers::span<const uint8_t, 16>(bytes));
  return out;
}

template <class In>
auto own_turn(const In* in) -> glow::TurnT {
  if (!in || !in->state() || !in->view())
    throw tsugumi::Failure(Tsugumi::Data::FailureKind::input_missing, "the Turn has no state or view");
  glow::TurnT turn;
  turn.state = own_state(in->state());
  turn.view = own_view(in->view());
  return turn;
}

inline auto make_turn(std::unique_ptr<glow::GameStateT> state, const glow::ViewT& view)
    -> glow::TurnT {
  glow::TurnT turn;
  turn.state = std::move(state);
  turn.view = std::make_unique<glow::ViewT>();
  turn.view->session = std::make_unique<glow::ViewSession>(*view.session);
  return turn;
}

inline auto session_of(const glow::ViewT& view) -> tsugumi_view_session {
  tsugumi_view_session session{};
  for (uint32_t i = 0; i < 16; ++i) session.bytes[i] = view.session->bytes()->Get(i);
  return session;
}

inline auto view_of(const tsugumi_view_session& session) -> std::unique_ptr<glow::ViewT> {
  auto view = std::make_unique<glow::ViewT>();
  view->session = std::make_unique<glow::ViewSession>(
      flatbuffers::span<const uint8_t, 16>(session.bytes));
  return view;
}

// Short numbers: 950, 12.3K, 4.56M, 7.89B, then 1.2e15. The view formats its
// counters the same way.
inline auto number(double value) -> std::string {
  char text[32];
  if (!(value >= 0)) value = 0;
  if (value < 1000) {
    std::snprintf(text, sizeof text, "%.0f", std::floor(value));
    return text;
  }
  static constexpr const char* kUnits[] = {"K", "M", "B", "T"};
  double scaled = value;
  int unit = -1;
  while (scaled >= 1000 && unit < 3) {
    scaled /= 1000;
    ++unit;
  }
  if (scaled >= 1000) {
    std::snprintf(text, sizeof text, "%.1e", value);
    return text;
  }
  const int digits = scaled < 10 ? 2 : scaled < 100 ? 1 : 0;
  const double step = std::pow(10.0, digits);
  std::snprintf(text, sizeof text, "%.*f%s", digits, std::floor(scaled * step) / step, kUnits[unit]);
  return text;
}

inline auto replace(std::string text, std::string_view key, std::string_view value) -> std::string {
  for (auto at = text.find(key); at != std::string::npos; at = text.find(key, at + value.size()))
    text.replace(at, key.size(), value);
  return text;
}

inline auto clock_text(uint64_t ms) -> std::string {
  char text[32];
  const uint64_t seconds = ms / 1000;
  std::snprintf(text, sizeof text, "%llu:%02llu", static_cast<unsigned long long>(seconds / 60),
                static_cast<unsigned long long>(seconds % 60));
  return text;
}

}  // namespace game
