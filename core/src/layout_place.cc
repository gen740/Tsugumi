// Placing a scope's Cards, output frames and notes (see layout.hh).
//
// The graph is drawn in layers along the flow. Wires against the flow are set
// aside, each node gets a layer, wires that pass a layer get a point in it,
// the order in each layer is the one with the fewest crossings found, and the
// positions across the flow are those that bend the wires least (a network
// simplex). Graphs that have nothing to do with each other are laid out apart,
// then frames and notes take the free room nearest to what they belong to.
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>

#include "layout.hh"

namespace tsugumi::layout {
namespace {

// Space between Cards side by side, and more than this between layers.
constexpr double kSideGap = 40;
constexpr double kLayerGap = 30;
// Room a wire passing a layer keeps from the Cards and wires beside it.
constexpr double kLaneGap = 30;
// Room around frames and notes, and around wires they sit next to.
constexpr double kBoxGap = 20;
constexpr double kWireRoom = 12;
// What a frame on the other side of its Card costs (it may be free and near).
constexpr double kFrameOtherSide = 400;
// How much a straight wire counts: a long wire bending is the worst to read.
constexpr int kWeightFeedback = 1;
constexpr int kWeightNear = 4;
constexpr int kWeightLongEnd = 8;
constexpr int kWeightLong = 32;
// Sweeps of the ordering, and the longest search of the coordinates.
constexpr int kOrderSweeps = 24;
constexpr int kSimplexLimit = 4000;
// A row of unconnected graphs is at least this wide before it wraps.
constexpr double kShelfMin = 1200;
constexpr double kInfinity = std::numeric_limits<double>::infinity();

// A Card, a bar of a Card in Card, or a point of a wire passing a layer.
struct Node {
  int card = -1;  // index in the scene, -1 otherwise
  bool bar = false;
  double across = 0;        // size across the flow
  double along = 0;         // size along it
  double frame_across = 0;  // the output frame that goes with it (0: none)
  double frame_along = 0;
  std::vector<double> inputs;  // where its ports are across the flow
  std::vector<double> outputs;
  int rank = 0;
  int order = 0;
  double at = 0;        // position across the flow
  double layer_at = 0;  // position along it
};

// A wire in flow direction. `feedback`: it runs against the flow.
struct Edge {
  int from = 0;
  int from_slot = 0;
  int to = 0;
  int to_slot = 0;
  bool feedback = false;
};

// One piece of a wire, between neighbouring layers.
struct Piece {
  int from = 0;
  double from_at = 0;
  int to = 0;
  double to_at = 0;
  int weight = 0;
};

// In the frame of the flow: `across` it and `along` it.
struct FlowRect {
  double across = 0;
  double along = 0;
  double across_size = 0;
  double along_size = 0;
};

// From (a, b) to (c, d): across, along.
struct Line {
  double a = 0;
  double b = 0;
  double c = 0;
  double d = 0;
};

struct Arc {
  int from = 0;
  int to = 0;
  long long min = 0;
  int weight = 0;
};

auto round_near(double v) -> double { return std::floor(v + 0.5); }
auto ceil_to(double v, double g) -> double {
  return std::ceil(v / g - 1e-9) * g;
}
auto floor_to(double v, double g) -> double {
  return std::floor(v / g + 1e-9) * g;
}
auto near_to(double v, double g) -> double { return round_near(v / g) * g; }
auto tenth(double v) -> double { return round_near(v * 10) / 10 + 0; }

auto union_rect(const FlowRect& r, const FlowRect& o) -> FlowRect {
  const double lo = std::min(r.across, o.across);
  const double top = std::min(r.along, o.along);
  return {lo, top,
          std::max(r.across + r.across_size, o.across + o.across_size) - lo,
          std::max(r.along + r.along_size, o.along + o.along_size) - top};
}

auto hits(const FlowRect& r, const FlowRect& o, double gap) -> bool {
  return r.across < o.across + o.across_size + gap &&
         o.across < r.across + r.across_size + gap &&
         r.along < o.along + o.along_size + gap &&
         o.along < r.along + r.along_size + gap;
}

// Whether a segment comes within `room` of a rectangle.
auto line_hits(const Line& l, const FlowRect& r, double room) -> bool {
  const double lo = r.across - room;
  const double hi = r.across + r.across_size + room;
  const double top = r.along - room;
  const double bottom = r.along + r.along_size + room;
  // Clipped to the rectangle along its length (Liang-Barsky).
  double t0 = 0;
  double t1 = 1;
  const double dx = l.c - l.a;
  const double dy = l.d - l.b;
  auto clip = [&](double p, double q) {
    if (p == 0) return q >= 0;
    const double t = q / p;
    if (p < 0) {
      if (t > t1) return false;
      if (t > t0) t0 = t;
    } else {
      if (t < t0) return false;
      if (t < t1) t1 = t;
    }
    return true;
  };
  return clip(-dx, l.a - lo) && clip(dx, hi - l.a) && clip(-dy, l.b - top) &&
         clip(dy, bottom - l.b);
}

auto sorted_unique(std::vector<double> list) -> std::vector<double> {
  std::sort(list.begin(), list.end());
  list.erase(std::unique(list.begin(), list.end()), list.end());
  return list;
}

// The free place for a box nearest `near` that grows the drawing the least,
// on the side frames open to (right of a Card when the flow runs down).
auto place_box(double across_size, double along_size, const FlowRect& near,
               bool frame, const std::vector<FlowRect>& obstacles,
               const std::vector<Line>& lines, double grid) -> FlowRect {
  std::vector<double> across_at = {
      ceil_to(near.across + near.across_size + kBoxGap, grid),
      floor_to(near.across - across_size - kBoxGap, grid)};
  std::vector<double> along_at = {
      near_to(near.along, grid),
      near_to(near.along + near.along_size - along_size, grid)};
  for (const auto& o : obstacles) {
    across_at.push_back(ceil_to(o.across + o.across_size + kBoxGap, grid));
    across_at.push_back(floor_to(o.across - across_size - kBoxGap, grid));
    along_at.push_back(near_to(o.along, grid));
    along_at.push_back(near_to(o.along + o.along_size - along_size, grid));
    along_at.push_back(ceil_to(o.along + o.along_size + kBoxGap, grid));
    along_at.push_back(floor_to(o.along - along_size - kBoxGap, grid));
  }
  for (const auto& l : lines) {
    across_at.push_back(ceil_to(std::max(l.a, l.c) + kWireRoom, grid));
    across_at.push_back(
        floor_to(std::min(l.a, l.c) - across_size - kWireRoom, grid));
  }
  const auto across = sorted_unique(std::move(across_at));
  const auto along = sorted_unique(std::move(along_at));
  std::optional<FlowRect> box;
  for (const auto& o : obstacles) box = box ? union_rect(*box, o) : o;
  std::optional<FlowRect> best;
  double best_cost = kInfinity;
  for (const double a : along) {
    for (const double c : across) {
      const FlowRect r{c, a, across_size, along_size};
      if (std::any_of(
              obstacles.begin(), obstacles.end(),
              [&](const FlowRect& o) { return hits(r, o, kBoxGap - 1e-6); }))
        continue;
      if (std::any_of(lines.begin(), lines.end(), [&](const Line& l) {
            return line_hits(l, r, kWireRoom);
          }))
        continue;
      const double gap_across =
          std::max({0.0, near.across - (c + across_size),
                    c - (near.across + near.across_size)});
      const double gap_along = std::max({0.0, near.along - (a + along_size),
                                         a - (near.along + near.along_size)});
      double growth = 0;
      if (box) {
        const auto u = union_rect(*box, r);
        growth =
            u.across_size - box->across_size + (u.along_size - box->along_size);
      }
      // A frame opens to one side of its Card and stays level with it if it
      // can; a note has no Card and goes towards where the flow ends.
      const double side =
          frame ? (c + across_size / 2 < near.across + near.across_size / 2
                       ? kFrameOtherSide
                       : 0) +
                      0.1 * std::abs(a - near.along)
                : -0.01 * a + 0.001 * c;
      const double cost = 2 * (gap_across + gap_along) + growth + side;
      if (cost < best_cost - 1e-9) {
        best_cost = cost;
        best = r;
      }
    }
  }
  // (Past everything there is always room.)
  return best ? *best
              : FlowRect{across.back(), along.front(), across_size, along_size};
}

// Network simplex: minimises the sum of weight * (x[to] - x[from]) with
// x[to] - x[from] >= min, over whole numbers.
auto solve_network(int count, const std::vector<Arc>& arcs)
    -> std::vector<long long> {
  std::vector<long long> x(static_cast<size_t>(count), 0);
  if (count == 0) return x;
  // A first solution: every variable as low as its arcs allow.
  for (int pass = 0; pass <= count; pass++) {
    bool changed = false;
    for (const auto& a : arcs) {
      if (x[a.to] < x[a.from] + a.min) {
        x[a.to] = x[a.from] + a.min;
        changed = true;
      }
    }
    if (!changed) break;
  }
  auto slack = [&](const Arc& a) { return x[a.to] - x[a.from] - a.min; };
  // A spanning tree of tight arcs.
  std::vector<char> in_tree(static_cast<size_t>(count), 0);
  std::vector<int> tree;
  in_tree[0] = 1;
  int size = 1;
  while (size < count) {
    bool grew = false;
    for (size_t i = 0; i < arcs.size(); i++) {
      const auto& a = arcs[i];
      if (in_tree[a.from] != in_tree[a.to] && slack(a) == 0) {
        in_tree[a.from] = 1;
        in_tree[a.to] = 1;
        tree.push_back(static_cast<int>(i));
        size++;
        grew = true;
      }
    }
    if (grew) continue;
    int best = -1;
    long long best_slack = LLONG_MAX;
    for (size_t i = 0; i < arcs.size(); i++) {
      const auto& a = arcs[i];
      if (in_tree[a.from] != in_tree[a.to] && slack(a) < best_slack) {
        best = static_cast<int>(i);
        best_slack = slack(a);
      }
    }
    if (best < 0) {
      // Not connected (does not happen for one graph): carry on from the next
      // node.
      *std::find(in_tree.begin(), in_tree.end(), 0) = 1;
      size++;
      continue;
    }
    const long long delta = in_tree[arcs[best].from] ? best_slack : -best_slack;
    for (int v = 0; v < count; v++)
      if (in_tree[v]) x[v] += delta;
  }
  // Exchanges: an arc of the tree whose cut would gain from growing leaves it.
  std::vector<std::vector<int>> adjacent(static_cast<size_t>(count));
  auto rebuild = [&] {
    for (auto& l : adjacent) l.clear();
    for (const int t : tree) {
      adjacent[arcs[t].from].push_back(arcs[t].to);
      adjacent[arcs[t].to].push_back(arcs[t].from);
    }
  };
  rebuild();
  std::vector<char> side(static_cast<size_t>(count), 0);
  std::vector<int> stack;
  auto flood = [&](int start, const Arc& without) {
    std::fill(side.begin(), side.end(), 0);
    side[start] = 1;
    stack.assign(1, start);
    while (!stack.empty()) {
      const int v = stack.back();
      stack.pop_back();
      for (const int w : adjacent[v]) {
        if (side[w]) continue;
        if ((v == without.from && w == without.to) ||
            (v == without.to && w == without.from))
          continue;
        side[w] = 1;
        stack.push_back(w);
      }
    }
  };
  for (int step = 0; step < kSimplexLimit; step++) {
    int leaving = -1;
    for (size_t k = 0; k < tree.size() && leaving < 0; k++) {
      const Arc e = arcs[tree[k]];
      flood(e.from, e);  // `side`: the part with the arc's start
      long long cut = 0;
      for (const auto& a : arcs) {
        if (side[a.from] && !side[a.to]) {
          cut += a.weight;
        } else if (!side[a.from] && side[a.to]) {
          cut -= a.weight;
        }
      }
      if (cut < 0) leaving = static_cast<int>(k);
    }
    if (leaving < 0) break;
    int entering = -1;
    long long best_slack = LLONG_MAX;
    for (size_t i = 0; i < arcs.size(); i++) {
      const auto& a = arcs[i];
      if (!side[a.from] && side[a.to] && slack(a) < best_slack) {
        entering = static_cast<int>(i);
        best_slack = slack(a);
      }
    }
    if (entering < 0) break;
    for (int v = 0; v < count; v++)
      if (!side[v]) x[v] += best_slack;
    tree[leaving] = entering;
    rebuild();
  }
  const long long low = *std::min_element(x.begin(), x.end());
  for (auto& v : x) v -= low;
  return x;
}

// For each wire, the nodes it passes (its two ends and the points between).
struct Chain {
  int edge = 0;
  std::vector<int> path;
};

// One connected graph: layers, the order in them, and the coordinates.
class Layered {
 public:
  Layered(std::vector<Node>& nodes, std::vector<Edge> edges,
          std::vector<int>& list, double grid, bool snap)
      : n_(nodes),
        edges_(std::move(edges)),
        list_(list),
        grid_(grid),
        snap_(snap) {}

  // Lays the graph out, and says where its wires run.
  auto run() -> std::vector<Line> {
    remove_cycles();
    assign_ranks();
    add_lanes();
    order_layers();
    assign_across();
    assign_along();
    return wire_lines();
  }

 private:
  // Wires against the flow: those that close a cycle, seen from the sources.
  auto remove_cycles() -> void {
    std::vector<char> state(n_.size(), 0);
    std::vector<char> has_in(n_.size(), 0);
    for (const auto& e : edges_) has_in[e.to] = 1;
    struct Frame {
      int node;
      size_t next;
    };
    auto visit = [&](int root) {
      std::vector<Frame> stack = {{root, 0}};
      state[root] = 1;
      while (!stack.empty()) {
        const size_t top = stack.size() - 1;
        bool pushed = false;
        while (stack[top].next < edges_.size()) {
          auto& e = edges_[stack[top].next++];
          if (e.from != stack[top].node || e.feedback) continue;
          const char s = state[e.to];
          if (s == 1) {
            e.feedback = true;
          } else if (s == 0) {
            state[e.to] = 1;
            stack.push_back({e.to, 0});
            pushed = true;
            break;
          }
        }
        if (!pushed) {
          state[stack[top].node] = 2;
          stack.pop_back();
        }
      }
    };
    for (const int i : list_)
      if (!has_in[i] && state[i] == 0) visit(i);
    for (const int i : list_)
      if (state[i] == 0) visit(i);
  }

  [[nodiscard]] static auto before(const Edge& e) -> int {
    return e.feedback ? e.to : e.from;
  }
  [[nodiscard]] static auto after(const Edge& e) -> int {
    return e.feedback ? e.from : e.to;
  }

  // The layer of each node: after everything before it, and sources (or
  // nodes that give to more than they take) as late as their wires allow.
  auto assign_ranks() -> void {
    for (const int i : list_) n_[i].rank = 0;
    for (size_t pass = 0; pass <= list_.size(); pass++) {
      bool changed = false;
      for (const auto& e : edges_) {
        if (n_[after(e)].rank < n_[before(e)].rank + 1) {
          n_[after(e)].rank = n_[before(e)].rank + 1;
          changed = true;
        }
      }
      if (!changed) break;
    }
    for (size_t pass = 0; pass < list_.size(); pass++) {
      bool changed = false;
      for (size_t k = list_.size(); k-- > 0;) {
        const int i = list_[k];
        if (n_[i].bar) continue;
        int outs = 0;
        int ins = 0;
        int room = INT_MAX;
        for (const auto& e : edges_) {
          if (before(e) == i) {
            outs++;
            room = std::min(room, n_[after(e)].rank - 1);
          }
          if (after(e) == i) ins++;
        }
        if (outs == 0 || ins >= outs) continue;
        if (room > n_[i].rank) {
          n_[i].rank = room;
          changed = true;
        }
      }
      if (!changed) break;
    }
    // The bars stay outside the inner Cards.
    int lo = INT_MAX;
    int hi = INT_MIN;
    for (const int i : list_) {
      if (n_[i].bar) continue;
      lo = std::min(lo, n_[i].rank);
      hi = std::max(hi, n_[i].rank);
    }
    for (const int i : list_) {
      if (n_[i].bar) n_[i].rank = n_[i].inputs.empty() ? lo - 1 : hi + 1;
    }
    int first = INT_MAX;
    for (const int i : list_) first = std::min(first, n_[i].rank);
    for (const int i : list_) n_[i].rank -= first;
  }

  // Points on the wires that pass a layer, so they have a lane through it.
  auto add_lanes() -> void {
    int last = 0;
    for (const int i : list_) last = std::max(last, n_[i].rank);
    layers_.assign(static_cast<size_t>(last) + 1, {});
    for (const int i : list_) layers_[n_[i].rank].push_back(i);
    for (size_t k = 0; k < edges_.size(); k++) {
      const auto& e = edges_[k];
      if (e.feedback) {
        chains_.push_back({static_cast<int>(k), {e.from, e.to}});
        continue;
      }
      std::vector<int> path = {e.from};
      for (int r = n_[e.from].rank + 1; r < n_[e.to].rank; r++) {
        const int d = static_cast<int>(n_.size());
        Node lane;
        lane.rank = r;
        n_.push_back(std::move(lane));
        list_.push_back(d);
        layers_[r].push_back(d);
        path.push_back(d);
      }
      path.push_back(e.to);
      chains_.push_back({static_cast<int>(k), std::move(path)});
    }
    renumber();
  }

  auto renumber() -> void {
    for (const auto& l : layers_)
      for (size_t k = 0; k < l.size(); k++)
        n_[l[k]].order = static_cast<int>(k);
  }

  // Where on its node (0 to 1 across it) a piece of a chain starts and ends.
  [[nodiscard]] auto start_port(const Chain& c, size_t k) const -> double {
    const auto& e = edges_[c.edge];
    return k == 0 ? (e.from_slot + 0.5) / static_cast<double>(std::max<size_t>(
                                              n_[c.path[k]].outputs.size(), 1))
                  : 0.5;
  }
  [[nodiscard]] auto end_port(const Chain& c, size_t k) const -> double {
    const auto& e = edges_[c.edge];
    return k + 2 == c.path.size()
               ? (e.to_slot + 0.5) / static_cast<double>(std::max<size_t>(
                                         n_[c.path[k + 1]].inputs.size(), 1))
               : 0.5;
  }

  struct Link {
    double top;
    double bottom;
  };

  // Position in the layer and on the port, at both ends of each piece between
  // two layers.
  [[nodiscard]] auto links(int rank) const -> std::vector<Link> {
    std::vector<Link> out;
    for (const auto& c : chains_) {
      if (edges_[c.edge].feedback) continue;
      for (size_t k = 0; k + 1 < c.path.size(); k++) {
        const int a = c.path[k];
        const int b = c.path[k + 1];
        if (n_[a].rank != rank) continue;
        out.push_back(
            {n_[a].order + start_port(c, k), n_[b].order + end_port(c, k)});
      }
    }
    return out;
  }

  [[nodiscard]] auto crossings() const -> int {
    int total = 0;
    for (size_t r = 0; r + 1 < layers_.size(); r++) {
      const auto l = links(static_cast<int>(r));
      for (size_t i = 0; i < l.size(); i++) {
        for (size_t j = i + 1; j < l.size(); j++) {
          const double top = l[i].top - l[j].top;
          const double bottom = l[i].bottom - l[j].bottom;
          if ((top < 0 && bottom > 0) || (top > 0 && bottom < 0)) total++;
        }
      }
    }
    return total;
  }

  // Barycentres, sweeping down and up, keeping the order with the fewest
  // crossings.
  auto order_layers() -> void {
    auto best = layers_;
    int best_count = crossings();
    for (int sweep = 0; sweep < kOrderSweeps && best_count > 0; sweep++) {
      const bool down = sweep % 2 == 0;
      const size_t count = layers_.size();
      for (size_t k = 1; k < count; k++)
        sort_layer(down ? k : count - 1 - k, down);
      const int c = crossings();
      if (c < best_count) {
        best_count = c;
        best = layers_;
      }
    }
    layers_ = best;
    renumber();
    // Neighbours that are better the other way round.
    for (int pass = 0; pass < 8 && best_count > 0; pass++) {
      bool improved = false;
      for (auto& l : layers_) {
        for (size_t k = 0; k + 1 < l.size(); k++) {
          const int a = l[k];
          const int b = l[k + 1];
          l[k] = b;
          l[k + 1] = a;
          n_[a].order = static_cast<int>(k) + 1;
          n_[b].order = static_cast<int>(k);
          const int c = crossings();
          if (c < best_count) {
            best_count = c;
            improved = true;
          } else {
            l[k] = a;
            l[k + 1] = b;
            n_[a].order = static_cast<int>(k);
            n_[b].order = static_cast<int>(k) + 1;
          }
        }
      }
      if (!improved) break;
    }
  }

  auto sort_layer(size_t r, bool down) -> void {
    struct Keyed {
      int id;
      size_t k;
      double v;
    };
    std::vector<Keyed> keyed;
    auto& l = layers_[r];
    for (size_t position = 0; position < l.size(); position++) {
      const int id = l[position];
      double sum = 0;
      int count = 0;
      for (const auto& c : chains_) {
        if (edges_[c.edge].feedback) continue;
        for (size_t k = 0; k + 1 < c.path.size(); k++) {
          const int a = c.path[k];
          const int b = c.path[k + 1];
          if (down && b == id) {
            sum += n_[a].order + start_port(c, k);
            count++;
          } else if (!down && a == id) {
            sum += n_[b].order + end_port(c, k);
            count++;
          }
        }
      }
      keyed.push_back(
          {id, position, count > 0 ? sum / count : n_[id].order + 0.5});
    }
    std::stable_sort(keyed.begin(), keyed.end(),
                     [](const Keyed& a, const Keyed& b) { return a.v < b.v; });
    for (size_t k = 0; k < keyed.size(); k++) {
      l[k] = keyed[k].id;
      n_[keyed[k].id].order = static_cast<int>(k);
    }
  }

  [[nodiscard]] auto port(int id, const std::vector<double>& list,
                          int slot) const -> double {
    return static_cast<size_t>(slot) < list.size() ? list[slot]
                                                   : n_[id].across / 2;
  }

  // Positions across the flow: the least (weighted) bending of the wires that
  // keeps the order and the room in each layer.
  auto assign_across() -> void {
    pieces_.clear();
    for (const auto& c : chains_) {
      const auto& e = edges_[c.edge];
      const size_t last = c.path.size() - 1;
      for (size_t k = 0; k < last; k++) {
        const int a = c.path[k];
        const int b = c.path[k + 1];
        const double from_at = k == 0 ? port(a, n_[a].outputs, e.from_slot) : 0;
        const double to_at =
            k + 1 == last ? port(b, n_[b].inputs, e.to_slot) : 0;
        const bool lane_a = k > 0;
        const bool lane_b = k + 1 < last;
        const int weight = e.feedback         ? kWeightFeedback
                           : lane_a && lane_b ? kWeightLong
                           : lane_a || lane_b ? kWeightLongEnd
                                              : kWeightNear;
        pieces_.push_back({a, from_at, b, to_at, weight});
      }
    }
    // The network: one variable per node, one more per piece of wire, in whole
    // grids when Cards are on it (a wire whose ports are not a whole number of
    // grids apart stays as near as it can).
    const double unit = snap_ ? grid_ : 1;
    const std::vector<int> vars = list_;
    std::vector<int> index(n_.size(), -1);
    for (size_t i = 0; i < vars.size(); i++)
      index[vars[i]] = static_cast<int>(i);
    std::vector<Arc> arcs;
    for (const auto& l : layers_) {
      for (size_t k = 0; k + 1 < l.size(); k++) {
        const auto& a = n_[l[k]];
        const auto& b = n_[l[k + 1]];
        const bool lane = a.across == 0 || b.across == 0;
        // A Card keeps the room for its frame beside it.
        const double frame =
            a.frame_across > 0 ? a.frame_across + kBoxGap + grid_ : 0;
        const double room = std::ceil(
            (a.across + frame + (lane ? kLaneGap : kSideGap)) / unit - 1e-9);
        arcs.push_back(
            {index[l[k]], index[l[k + 1]], static_cast<long long>(room), 0});
      }
    }
    int count = static_cast<int>(vars.size());
    for (const auto& p : pieces_) {
      const auto apart =
          static_cast<long long>(round_near((p.from_at - p.to_at) / unit));
      const int aux = count++;
      arcs.push_back({aux, index[p.from], apart >= 0 ? 0 : -apart, p.weight});
      arcs.push_back({aux, index[p.to], apart >= 0 ? apart : 0, p.weight});
    }
    const auto solved = solve_network(count, arcs);
    for (size_t i = 0; i < vars.size(); i++)
      n_[vars[i]].at = static_cast<double>(solved[i]) * unit;
    if (!snap_) settle_straight(index);
  }

  // Without the grid: wires that came out straight stay exactly straight (ports
  // are not whole numbers), and each straight run moves as one, most of its
  // Cards on the lines of the grid.
  auto settle_straight(const std::vector<int>& index) -> void {
    std::vector<int> parent(list_.size());
    for (size_t i = 0; i < parent.size(); i++) parent[i] = static_cast<int>(i);
    auto find = [&](int i) {
      while (parent[i] != i) {
        parent[i] = parent[parent[i]];
        i = parent[i];
      }
      return i;
    };
    std::vector<Piece> straight;
    for (const auto& p : pieces_) {
      if (p.weight != kWeightFeedback &&
          n_[p.to].at - n_[p.from].at == round_near(p.from_at - p.to_at))
        straight.push_back(p);
    }
    for (const auto& p : straight) {
      const int a = find(index[p.from]);
      const int b = find(index[p.to]);
      if (a != b) parent[std::max(a, b)] = std::min(a, b);
    }
    std::vector<std::vector<int>> groups(list_.size());
    for (size_t i = 0; i < list_.size(); i++)
      groups[find(static_cast<int>(i))].push_back(list_[i]);
    for (const auto& group : groups) {
      if (group.empty()) continue;
      // Exact positions from the first member, following its straight wires.
      std::vector<char> seen(n_.size(), 0);
      seen[group[0]] = 1;
      std::vector<int> queue = {group[0]};
      for (size_t head = 0; head < queue.size(); head++) {
        const int u = queue[head];
        for (const auto& p : straight) {
          if (p.from == u && !seen[p.to]) {
            n_[p.to].at = n_[u].at + p.from_at - p.to_at;
            seen[p.to] = 1;
            queue.push_back(p.to);
          } else if (p.to == u && !seen[p.from]) {
            n_[p.from].at = n_[u].at + p.to_at - p.from_at;
            seen[p.from] = 1;
            queue.push_back(p.from);
          }
        }
      }
      // The shift that puts the most Cards on the grid, the smallest of those.
      double best_shift = 0;
      double best_score = -1;
      std::vector<int> cards;
      for (const int id : group)
        if (n_[id].card >= 0) cards.push_back(id);
      for (const int id : cards.empty() ? group : cards) {
        const double shift = near_to(n_[id].at, grid_) - n_[id].at;
        double score = 0;
        for (const int o : cards) {
          const double v = (n_[o].at + shift) / grid_;
          if (std::abs(v - round_near(v)) < 1e-6) score += n_[o].across;
        }
        if (score > best_score + 1e-9 ||
            (std::abs(score - best_score) <= 1e-9 &&
             std::abs(shift) < std::abs(best_shift) - 1e-9)) {
          best_score = score;
          best_shift = shift;
        }
      }
      for (const int id : group) n_[id].at += best_shift;
    }
  }

  // Positions along the flow: each layer just past the one before it, and
  // past the frames of earlier layers that reach it.
  auto assign_along() -> void {
    struct Reach {
      double lo;
      double hi;
      double end;
    };
    double at = 0;
    std::vector<Reach> reach;
    for (const auto& l : layers_) {
      double size = 0;
      bool any = false;
      for (const int id : l) {
        if (n_[id].card < 0) continue;
        size = std::max(size, n_[id].along);
        any = true;
      }
      for (const int id : l) {
        const auto& v = n_[id];
        if (v.card < 0) continue;
        const double spans[2][2] = {
            {v.at, v.at + v.across},
            {v.at + v.across + kBoxGap,
             v.at + v.across + kBoxGap + v.frame_across}};
        for (int s = 0; s < (v.frame_across > 0 ? 2 : 1); s++) {
          for (const auto& r : reach) {
            if (spans[s][0] < r.hi + kBoxGap && r.lo < spans[s][1] + kBoxGap)
              at = std::max(at, ceil_to(r.end + kBoxGap, grid_));
          }
        }
      }
      for (const int id : l) {
        auto& v = n_[id];
        v.layer_at = at;
        if (v.frame_across > 0)
          reach.push_back({v.at + v.across + kBoxGap,
                           v.at + v.across + kBoxGap + v.frame_across,
                           at + v.frame_along});
      }
      if (any) at = ceil_to(at + size + kLayerGap, grid_);
    }
  }

  [[nodiscard]] auto wire_lines() const -> std::vector<Line> {
    std::vector<Line> lines;
    struct At {
      double across;
      double along;
    };
    auto point = [&](const Chain& c, size_t k) -> At {
      const auto& e = edges_[c.edge];
      const int id = c.path[k];
      const auto& v = n_[id];
      if (k == 0)
        return {v.at + port(id, v.outputs, e.from_slot), v.layer_at + v.along};
      if (k == c.path.size() - 1)
        return {v.at + port(id, v.inputs, e.to_slot), v.layer_at};
      return {v.at, v.layer_at};
    };
    for (const auto& c : chains_) {
      if (edges_[c.edge].feedback) continue;
      for (size_t k = 0; k + 1 < c.path.size(); k++) {
        const At a = point(c, k);
        const At b = point(c, k + 1);
        lines.push_back({a.across, a.along, b.across, b.along});
      }
    }
    return lines;
  }

  std::vector<Node>& n_;
  std::vector<Edge> edges_;
  std::vector<int>& list_;
  double grid_;
  bool snap_;
  std::vector<std::vector<int>> layers_;
  std::vector<Piece> pieces_;
  std::vector<Chain> chains_;
};

struct FramePlace {
  int card;     // index in the scene
  size_t rect;  // index in the block's room
};

// One graph with its output frames.
struct Block {
  std::vector<int> list;
  std::vector<FramePlace> frames;
  std::vector<FlowRect> room;
  std::vector<Line> lines;
  FlowRect box;
};

}  // namespace

auto place(const Scene& scene) -> Proposal {
  const bool vertical = scene.top_to_bottom;
  const double grid = scene.grid > 0 ? scene.grid : 20;
  auto card_index = [&](const std::string& id) {
    for (size_t i = 0; i < scene.cards.size(); i++)
      if (scene.cards[i].id == id) return static_cast<int>(i);
    return -1;
  };
  auto slot = [](const std::vector<Port>& list, const std::string& name) {
    for (size_t i = 0; i < list.size(); i++)
      if (list[i].name == name) return static_cast<int>(i);
    return 0;
  };
  auto frame_of = [&](const std::string& id) -> const Box* {
    for (const auto& f : scene.frames)
      if (f.id == id) return &f;
    return nullptr;
  };

  std::vector<Node> nodes;
  for (size_t i = 0; i < scene.cards.size(); i++) {
    const auto& c = scene.cards[i];
    const Box* frame = frame_of(c.id);
    Node n;
    n.card = static_cast<int>(i);
    n.across = vertical ? c.width : c.height;
    n.along = vertical ? c.height : c.width;
    n.frame_across = frame ? (vertical ? frame->width : frame->height) : 0;
    n.frame_along = frame ? (vertical ? frame->height : frame->width) : 0;
    for (const auto& p : c.inputs) n.inputs.push_back(vertical ? p.x : p.y);
    for (const auto& p : c.outputs) n.outputs.push_back(vertical ? p.x : p.y);
    nodes.push_back(std::move(n));
  }

  std::vector<Edge> edges;
  for (const auto& w : scene.wires) {
    const int f = card_index(w.from);
    const int t = card_index(w.to);
    if (f < 0 || t < 0 || f == t) continue;
    edges.push_back({f, slot(scene.cards[f].outputs, w.from_port), t,
                     slot(scene.cards[t].inputs, w.to_port), false});
  }

  // Bars are drawn above and below the inner Cards when the flow runs down:
  // they take part in the order, and wires from them get a lane.
  if (vertical) {
    auto add_bar = [&](const std::vector<BarEnd>& list, bool top) {
      if (std::none_of(list.begin(), list.end(), [&](const BarEnd& e) {
            return card_index(e.card) >= 0;
          }))
        return;
      const double width = static_cast<double>(list.size()) * 110 + 40;
      std::vector<double> slots;
      for (size_t k = 0; k < list.size(); k++)
        slots.push_back((width * static_cast<double>(k + 1)) /
                        static_cast<double>(list.size() + 1));
      const int bar = static_cast<int>(nodes.size());
      Node n;
      n.bar = true;
      n.across = width;
      (top ? n.outputs : n.inputs) = slots;
      nodes.push_back(std::move(n));
      for (size_t k = 0; k < list.size(); k++) {
        const int c = card_index(list[k].card);
        if (c < 0) continue;
        if (top) {
          edges.push_back({bar, static_cast<int>(k), c,
                           slot(scene.cards[c].inputs, list[k].port), false});
        } else {
          edges.push_back({c, slot(scene.cards[c].outputs, list[k].port), bar,
                           static_cast<int>(k), false});
        }
      }
    };
    add_bar(scene.inputs.ends, true);
    add_bar(scene.outputs.ends, false);
  }

  // Graphs that have nothing to do with each other are laid out apart.
  std::vector<int> parent(nodes.size());
  for (size_t i = 0; i < parent.size(); i++) parent[i] = static_cast<int>(i);
  auto find = [&](int i) {
    while (parent[i] != i) {
      parent[i] = parent[parent[i]];
      i = parent[i];
    }
    return i;
  };
  for (const auto& e : edges) {
    const int a = find(e.from);
    const int b = find(e.to);
    if (a != b) parent[std::max(a, b)] = std::min(a, b);
  }
  std::vector<int> roots;
  std::vector<std::vector<int>> members;
  for (size_t i = 0; i < nodes.size(); i++) {
    const int r = find(static_cast<int>(i));
    auto k = static_cast<size_t>(std::find(roots.begin(), roots.end(), r) -
                                 roots.begin());
    if (k == roots.size()) {
      roots.push_back(r);
      members.emplace_back();
    }
    members[k].push_back(static_cast<int>(i));
  }

  auto world_rect = [&](double x, double y, double w, double h) {
    return vertical ? FlowRect{x, y, w, h} : FlowRect{y, x, h, w};
  };
  // What a Card in Card draws above and below these Cards.
  auto bar_rects =
      [&](const std::vector<FlowRect>& cards) -> std::vector<FlowRect> {
    const size_t inputs = scene.inputs.ends.size();
    const size_t outputs = scene.outputs.ends.size();
    if ((inputs == 0 && outputs == 0) || cards.empty()) return {};
    FlowRect box = cards[0];
    for (const auto& r : cards) box = union_rect(box, r);
    const double x = vertical ? box.across : box.along;
    const double y = vertical ? box.along : box.across;
    const double w = vertical ? box.across_size : box.along_size;
    const double h = vertical ? box.along_size : box.across_size;
    const double width =
        std::max(w, static_cast<double>(std::max(inputs, outputs)) * 110 + 40);
    const double left = x + w / 2 - width / 2;
    return {world_rect(left, y - 96, width, 40),
            world_rect(left, y + h + 56, width, 40)};
  };
  auto card_rect = [&](int i) {
    return FlowRect{nodes[i].at, nodes[i].layer_at, nodes[i].across,
                    nodes[i].along};
  };

  // Each graph with its output frames, nearest the start of the flow first.
  std::vector<Block> blocks;
  for (auto& list : members) {
    std::vector<Edge> graph_edges;
    for (const auto& e : edges)
      if (std::find(list.begin(), list.end(), e.from) != list.end())
        graph_edges.push_back(e);
    Block b;
    b.lines =
        Layered(nodes, std::move(graph_edges), list, grid, scene.snap).run();
    std::vector<int> cards;
    for (const int i : list)
      if (nodes[i].card >= 0) cards.push_back(i);
    for (const int i : cards) b.room.push_back(card_rect(i));
    const auto bars = bar_rects(b.room);
    b.room.insert(b.room.end(), bars.begin(), bars.end());
    std::vector<int> owners;
    for (const int i : cards)
      if (nodes[i].frame_across > 0) owners.push_back(i);
    std::stable_sort(owners.begin(), owners.end(), [&](int x, int y) {
      return nodes[x].rank != nodes[y].rank ? nodes[x].rank < nodes[y].rank
                                            : nodes[x].order < nodes[y].order;
    });
    for (const int i : owners) {
      const Box& frame = *frame_of(scene.cards[nodes[i].card].id);
      b.frames.push_back({nodes[i].card, b.room.size()});
      b.room.push_back(place_box(vertical ? frame.width : frame.height,
                                 vertical ? frame.height : frame.width,
                                 card_rect(i), true, b.room, b.lines, grid));
    }
    b.box = b.room[0];
    for (const auto& r : b.room) b.box = union_rect(b.box, r);
    b.list = list;
    blocks.push_back(std::move(b));
  }

  // The biggest graph first, the others in rows beside it.
  std::vector<size_t> block_order(blocks.size());
  for (size_t i = 0; i < block_order.size(); i++) block_order[i] = i;
  std::stable_sort(block_order.begin(), block_order.end(),
                   [&](size_t a, size_t b) {
                     return blocks[a].list.size() > blocks[b].list.size();
                   });
  double area = 0;
  double shelf = kShelfMin;
  for (const auto& b : blocks) {
    area += (b.box.across_size + kSideGap) * (b.box.along_size + kLayerGap);
    shelf = std::max(shelf, b.box.across_size);
  }
  shelf = std::max(shelf, std::sqrt(area) * 1.4);
  double row_across = 0;
  double row_along = 0;
  double row_end = 0;
  std::vector<FlowRect> room;
  std::vector<Line> lines;
  for (const size_t k : block_order) {
    auto& b = blocks[k];
    if (row_across > 0 && row_across + b.box.across_size > shelf) {
      row_across = 0;
      row_along = ceil_to(row_end + kLayerGap + kSideGap, grid);
    }
    const double dc = row_across - floor_to(b.box.across, grid);
    const double da = row_along - floor_to(b.box.along, grid);
    for (const int i : b.list) {
      nodes[i].at += dc;
      nodes[i].layer_at += da;
    }
    for (auto& r : b.room) {
      r.across += dc;
      r.along += da;
      room.push_back(r);
    }
    for (const auto& l : b.lines)
      lines.push_back({l.a + dc, l.b + da, l.c + dc, l.d + da});
    row_across = ceil_to(row_across + b.box.across_size + kSideGap * 2, grid);
    row_end = std::max(row_end, b.box.along + da + b.box.along_size);
  }

  auto at = [&](const std::string& id, double across, double along) {
    return Place{id, tenth(vertical ? across : along),
                 tenth(vertical ? along : across)};
  };
  Proposal out;
  // Notes: free room next to everything else.
  for (const auto& note : scene.notes) {
    FlowRect box = room.empty() ? FlowRect{} : room[0];
    for (const auto& r : room) box = union_rect(box, r);
    const auto r = place_box(vertical ? note.width : note.height,
                             vertical ? note.height : note.width, box, false,
                             room, lines, grid);
    room.push_back(r);
    out.notes.push_back(at(note.id, r.across, r.along));
  }
  for (const auto& n : nodes)
    if (n.card >= 0)
      out.cards.push_back(at(scene.cards[n.card].id, n.at, n.layer_at));
  for (const auto& b : blocks)
    for (const auto& f : b.frames)
      out.frames.push_back(at(scene.cards[f.card].id, b.room[f.rect].across,
                              b.room[f.rect].along));
  return out;
}

}  // namespace tsugumi::layout
