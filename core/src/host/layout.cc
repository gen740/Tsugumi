// Arrange and Untangle Wires are pure operations on a measured document scope.
#include "layout.hh"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace tsugumi::layout {
namespace {
auto card_of(std::vector<Card>& cards, const std::string& id) -> Card* {
  auto i = std::find_if(cards.begin(), cards.end(),
                        [&](const Card& c) { return c.id == id; });
  return i == cards.end() ? nullptr : &*i;
}
auto port_of(const std::vector<Port>& ports, const std::string& name)
    -> const Port* {
  auto i = std::find_if(ports.begin(), ports.end(),
                        [&](const Port& p) { return p.name == name; });
  return i == ports.end() ? nullptr : &*i;
}
auto names(const std::vector<Port>& ports) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (const auto& p : ports) out.push_back(p.name);
  return out;
}
// Bars stay horizontal in both flow directions, as the inner canvas draws them.
auto with_bars(const Scene& scene, std::vector<Card>& cards,
               std::vector<Wire>& wires, bool placed) -> void {
  if (scene.inputs.ends.empty() && scene.outputs.ends.empty()) return;
  double left = 0, top = 0, right = 240, bottom = 120;
  if (!cards.empty()) {
    left = top = std::numeric_limits<double>::infinity();
    right = bottom = -std::numeric_limits<double>::infinity();
    for (const auto& c : cards) {
      left = std::min(left, c.x);
      top = std::min(top, c.y);
      right = std::max(right, c.x + c.width);
      bottom = std::max(bottom, c.y + c.height);
    }
  }
  const double width = std::max(
      right - left, static_cast<double>(std::max(scene.inputs.ends.size(),
                                                 scene.outputs.ends.size())) *
                            110 +
                        40);
  const double x = (left + right - width) / 2;
  for (bool output : {false, true}) {
    const auto& bar = output ? scene.outputs : scene.inputs;
    const std::string id =
        std::string(1, '\0') + (output ? "outputs" : "inputs");
    const auto at =
        placed && bar.at ? *bar.at : Point{x, output ? bottom + 56 : top - 96};
    Card c;
    c.id = id;
    c.x = at.x;
    c.y = at.y;
    c.width = width;
    c.height = 40;
    for (size_t i = 0; i < bar.ends.size(); ++i) {
      const auto& e = bar.ends[i];
      (output ? c.inputs : c.outputs)
          .push_back({e.name,
                      width * static_cast<double>(i + 1) /
                          static_cast<double>(bar.ends.size() + 1),
                      output ? 0.0 : 40.0});
      if (!e.card.empty() && !e.port.empty())
        wires.push_back(output ? Wire{e.card, e.port, id, e.name}
                               : Wire{id, e.name, e.card, e.port});
    }
    cards.push_back(std::move(c));
  }
}
auto orders(bool vertical, std::vector<Card> cards,
            const std::vector<Wire>& wires, const std::vector<Box>& frames)
    -> std::vector<PortOrder> {
  const auto original = cards;
  auto score = [&](std::vector<Card>& cs) {
    std::vector<WireEnds> ends;
    for (const auto& w : wires) {
      const auto* f = card_of(cs, w.from);
      const auto* t = card_of(cs, w.to);
      const auto* a = f ? port_of(f->outputs, w.from_port) : nullptr;
      const auto* b = t ? port_of(t->inputs, w.to_port) : nullptr;
      if (a && b)
        ends.push_back(
            {{f->x + a->x, f->y + a->y}, {t->x + b->x, t->y + b->y}});
    }
    std::vector<Rect> obstacles;
    for (const auto& c : cs) obstacles.push_back({c.x, c.y, c.width, c.height});
    for (const auto& f : frames)
      obstacles.push_back({f.x, f.y, f.width, f.height});
    return crossing_pairs(route_wires(vertical, obstacles, ends), ends);
  };
  int best = score(cards), trials = 0;
  bool done = false;
  for (int pass = 0; pass < 4 && best > 0 && !done; ++pass) {
    bool improved = false;
    for (bool output : {true, false}) {
      if (done) break;
      for (size_t i = 0; i < cards.size() && !done; ++i) {
        const auto& c = cards[i];
        if (!c.reorderable) continue;
        const auto& ports = output ? c.outputs : c.inputs;
        struct Attached {
          std::string name;
          size_t slot;
          double value;
        };
        std::vector<Attached> attached;
        for (size_t slot = 0; slot < ports.size(); ++slot) {
          const auto& p = ports[slot];
          double sum = 0;
          size_t count = 0;
          for (const auto& w : wires) {
            if (output ? w.from != c.id || w.from_port != p.name
                       : w.to != c.id || w.to_port != p.name)
              continue;
            const auto* other = card_of(cards, output ? w.to : w.from);
            const auto* port =
                other ? port_of(output ? other->inputs : other->outputs,
                                output ? w.to_port : w.from_port)
                      : nullptr;
            if (port) {
              sum += vertical ? other->x + port->x : other->y + port->y;
              ++count;
            }
          }
          if (count)
            attached.push_back(
                {p.name, slot, sum / static_cast<double>(count)});
        }
        auto sorted = attached;
        std::sort(sorted.begin(), sorted.end(),
                  [](const Attached& a, const Attached& b) {
                    return a.value != b.value ? a.value < b.value
                                              : a.slot < b.slot;
                  });
        bool same = true;
        for (size_t j = 0; j < attached.size(); ++j)
          if (attached[j].name != sorted[j].name) same = false;
        if (same) continue;
        if (trials++ >= 96) {
          done = true;
          break;
        }
        auto proposed = cards;
        auto& moved = output ? proposed[i].outputs : proposed[i].inputs;
        for (size_t j = 0; j < attached.size(); ++j)
          moved[attached[j].slot].name = sorted[j].name;
        const int count = score(proposed);
        if (count >= best) continue;
        cards = std::move(proposed);
        best = count;
        improved = true;
        if (best == 0) done = true;
      }
    }
    if (!improved) break;
  }
  std::vector<PortOrder> out;
  for (size_t i = 0; i < cards.size(); ++i) {
    auto ins = names(cards[i].inputs), outs = names(cards[i].outputs);
    if (ins != names(original[i].inputs) || outs != names(original[i].outputs))
      out.push_back({cards[i].id, std::move(ins), std::move(outs)});
  }
  return out;
}
}  // namespace

auto untangle(const Scene& scene, const std::vector<std::string>& around)
    -> Proposal {
  auto cards = scene.cards;
  auto wires = scene.wires;
  with_bars(scene, cards, wires, true);
  if (!around.empty()) {
    const std::set<std::string> moved(around.begin(), around.end());
    if (std::none_of(wires.begin(), wires.end(), [&](const Wire& w) {
          return moved.contains(w.from) != moved.contains(w.to);
        }))
      return {};
    auto neighbors = moved;
    for (const auto& w : wires) {
      if (moved.contains(w.from)) neighbors.insert(w.to);
      if (moved.contains(w.to)) neighbors.insert(w.from);
    }
    for (auto& c : cards)
      c.reorderable = c.reorderable && neighbors.contains(c.id);
  }
  Proposal out;
  out.port_orders =
      orders(scene.top_to_bottom, std::move(cards), wires, scene.frames);
  return out;
}
auto arrange(const Scene& scene) -> Proposal {
  const double grid = scene.grid > 0 ? scene.grid : 20;
  auto placed = place(scene);
  auto cards = scene.cards;
  auto wires = scene.wires;
  auto frames = scene.frames;
  for (auto& c : cards) {
    const auto p = std::find_if(placed.cards.begin(), placed.cards.end(),
                                [&](const Place& p) { return p.id == c.id; });
    c.x = p->x;
    c.y = p->y;
    if (scene.snap) {
      c.x = std::floor(c.x / grid + 0.5) * grid;
      c.y = std::floor(c.y / grid + 0.5) * grid;
    }
  }
  with_bars(scene, cards, wires, false);
  std::erase_if(frames, [&](Box& f) {
    const auto p = std::find_if(placed.frames.begin(), placed.frames.end(),
                                [&](const Place& p) { return p.id == f.id; });
    if (p == placed.frames.end()) return true;
    f.x = p->x;
    f.y = p->y;
    return false;
  });
  auto reordered = orders(scene.top_to_bottom, std::move(cards), wires, frames);
  if (!reordered.empty()) {
    auto input = scene;
    for (const auto& o : reordered) {
      auto* c = card_of(input.cards, o.id);
      for (size_t i = 0; i < c->inputs.size(); ++i)
        c->inputs[i].name = o.inputs[i];
      for (size_t i = 0; i < c->outputs.size(); ++i)
        c->outputs[i].name = o.outputs[i];
    }
    placed = place(input);
  }
  placed.port_orders = std::move(reordered);
  return placed;
}
}  // namespace tsugumi::layout
