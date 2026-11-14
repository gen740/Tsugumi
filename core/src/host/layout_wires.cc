// The router used to score port orders. Keep its arithmetic and tie breaking
// aligned with the presentation routers; wire-routes.json checks the paths.
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <queue>
#include <tuple>

#include "layout.hh"

namespace tsugumi::layout {
namespace {
constexpr double kClearance = 20;
constexpr double kLane = 12;
constexpr double kTurn = 30;
constexpr double kCrossing = 80;
constexpr double kOverlap = 3;
constexpr double kNear = 0.25;
constexpr double kPortDepth = 44;
constexpr double kInfinity = std::numeric_limits<double>::infinity();

auto equal(Point a, Point b) -> bool { return a.x == b.x && a.y == b.y; }
auto bounds(const std::vector<Point>& ps) -> Rect {
  double x = kInfinity, y = kInfinity, right = -kInfinity, bottom = -kInfinity;
  for (auto p : ps) {
    x = std::min(x, p.x);
    y = std::min(y, p.y);
    right = std::max(right, p.x);
    bottom = std::max(bottom, p.y);
  }
  return {x, y, right - x, bottom - y};
}
auto bezier(Point a, Point c1, Point c2, Point b, double t) -> Point {
  const double u = 1 - t;
  return {u * u * u * a.x + 3 * u * u * t * c1.x + 3 * u * t * t * c2.x +
              t * t * t * b.x,
          u * u * u * a.y + 3 * u * u * t * c1.y + 3 * u * t * t * c2.y +
              t * t * t * b.y};
}
auto segment_enters(Point p, Point q, Rect o) -> bool {
  double t0 = 0, t1 = 1;
  auto axis = [&](double from, double to, double lo, double hi) {
    const double d = to - from;
    if (d == 0) return from > lo && from < hi;
    const double ta = (lo - from) / d, tb = (hi - from) / d;
    t0 = std::max(t0, std::min(ta, tb));
    t1 = std::min(t1, std::max(ta, tb));
    return t0 <= t1;
  };
  return axis(p.x, q.x, o.x + 2, o.x + o.width - 2) &&
         axis(p.y, q.y, o.y + 2, o.y + o.height - 2);
}
auto sorted_unique(std::vector<double> xs) -> std::vector<double> {
  std::sort(xs.begin(), xs.end());
  xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
  return xs;
}
auto straightened(const std::vector<Point>& ps) -> std::vector<Point> {
  std::vector<Point> out;
  for (auto p : ps) {
    if (!out.empty() && equal(out.back(), p)) continue;
    const auto n = out.size();
    if (n >= 2 && ((out[n - 2].x == out[n - 1].x && out[n - 1].x == p.x) ||
                   (out[n - 2].y == out[n - 1].y && out[n - 1].y == p.y)))
      out.back() = p;
    else
      out.push_back(p);
  }
  return out;
}
struct Run {
  Point a;
  std::vector<Point> pts;
  Rect box;
  bool curved;
};
struct Near {
  std::vector<Rect> cards;
  std::vector<const Run*> wires;
};
class Router {
 public:
  Router(bool vertical, const std::vector<Rect>& obstacles)
      : vertical_(vertical), obstacles_(obstacles) {
    for (auto o : obstacles)
      frame_.push_back(vertical ? o : Rect{o.y, o.x, o.height, o.width});
  }
  auto route(const std::vector<WireEnds>& wires)
      -> std::vector<std::vector<Point>> {
    std::vector<std::vector<Point>> out(wires.size());
    for (size_t i = 0; i < wires.size(); ++i) {
      auto [a, b] = wires[i];
      if (((vertical_ ? b.y - a.y : b.x - a.x) >= 0 && !crosses_card(a, b)) ||
          under_cards(a, b))
        out[i] = curve(a, b);
      if (!out[i].empty()) {
        std::vector<Point> sampled;
        for (size_t k = 0; k < out[i].size(); k += 4)
          sampled.push_back(to_frame(out[i][k]));
        remember(a, std::move(sampled), true);
      }
    }
    std::vector<size_t> order;
    for (size_t i = 0; i < wires.size(); ++i)
      if (out[i].empty()) order.push_back(i);
    auto key = [&](size_t i) {
      auto [a, b] = wires[i];
      return std::tuple{std::abs(b.x - a.x) + std::abs(b.y - a.y), a.x, a.y,
                        b.x, b.y};
    };
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t a, size_t b) { return key(a) < key(b); });
    for (auto i : order) {
      auto [a, b] = wires[i];
      auto ps = search(to_frame(a), to_frame(b));
      if (ps.empty()) {
        out[i] = curve(a, b);
        continue;
      }
      remember(a, ps, false);
      for (auto p : ps) out[i].push_back(to_frame(p));
    }
    return out;
  }

 private:
  auto to_frame(Point p) const -> Point {
    return vertical_ ? p : Point{p.y, p.x};
  }
  auto controls(Point a, Point b) const -> std::pair<Point, Point> {
    if (vertical_) {
      const double d = std::max(std::abs(b.y - a.y) / 2,
                                std::min(30.0, std::abs(b.x - a.x) / 2));
      return {{a.x, a.y + d}, {b.x, b.y - d}};
    }
    const double d = std::max(std::abs(b.x - a.x) / 2,
                              std::min(30.0, std::abs(b.y - a.y) / 2));
    return {{a.x + d, a.y}, {b.x - d, b.y}};
  }
  auto curve(Point a, Point b) const -> std::vector<Point> {
    auto [c1, c2] = controls(a, b);
    std::vector<Point> out;
    for (int i = 0; i <= 32; ++i)
      out.push_back(bezier(a, c1, c2, b, static_cast<double>(i) / 32));
    return out;
  }
  auto crosses_card(Point a, Point b) const -> bool {
    auto [c1, c2] = controls(a, b);
    const double x0 = std::min({a.x, b.x, c1.x, c2.x}),
                 x1 = std::max({a.x, b.x, c1.x, c2.x});
    const double y0 = std::min({a.y, b.y, c1.y, c2.y}),
                 y1 = std::max({a.y, b.y, c1.y, c2.y});
    auto inside = [](Rect o, Point p) {
      return p.x > o.x + 2 && p.x < o.x + o.width - 2 && p.y > o.y + 2 &&
             p.y < o.y + o.height - 2;
    };
    std::vector<Rect> near;
    for (auto o : obstacles_)
      if (x0 < o.x + o.width && x1 > o.x && y0 < o.y + o.height && y1 > o.y &&
          !inside(o, a) && !inside(o, b))
        near.push_back(o);
    Point p = a;
    for (int i = 1; i <= 32; ++i) {
      const auto q = bezier(a, c1, c2, b, static_cast<double>(i) / 32);
      for (auto o : near)
        if (segment_enters(p, q, o)) return true;
      p = q;
    }
    return false;
  }
  auto under_cards(Point a, Point b) const -> bool {
    auto inside = [](Rect o, Point p) {
      return p.x > o.x && p.x < o.x + o.width && p.y > o.y &&
             p.y < o.y + o.height;
    };
    auto touches = [](Rect o, Point p) {
      return p.x >= o.x && p.x <= o.x + o.width && p.y >= o.y &&
             p.y <= o.y + o.height;
    };
    bool ab = false, ba = false;
    for (auto o : obstacles_) {
      ab = ab || (inside(o, a) && touches(o, b));
      ba = ba || (inside(o, b) && touches(o, a));
    }
    return ab && ba;
  }
  auto remember(Point a, std::vector<Point> ps, bool curved) -> void {
    auto box = bounds(ps);
    routed_.push_back({a, std::move(ps), box, curved});
  }
  static auto depth(Rect o, Point p, int dir) -> double {
    if (p.x <= o.x || p.x >= o.x + o.width || p.y <= o.y ||
        p.y >= o.y + o.height)
      return 0;
    return dir > 0 ? o.y + o.height - p.y : p.y - o.y;
  }
  static auto clear(Point p, int dir, const std::vector<Rect>& cards) -> Point {
    bool moved = true;
    while (moved) {
      moved = false;
      for (auto o : cards) {
        const double d = depth(o, p, dir);
        if (d > 0) {
          p.y += dir * d;
          moved = true;
        }
      }
    }
    return p;
  }
  static auto stub(Point p, int dir, double along,
                   const std::vector<Rect>& cards) -> double {
    double s = along > 0 ? std::min(kClearance, along / 2) : kClearance;
    for (auto o : cards) {
      if (p.x <= o.x || p.x >= o.x + o.width) continue;
      const double d = dir > 0 ? o.y - p.y : p.y - o.y - o.height;
      if (d >= 0) s = std::min(s, d / 2);
    }
    return s;
  }
  auto near_line(double v, bool horizontal, const std::vector<Rect>& cards,
                 const std::vector<const Run*>& wires) const -> Near {
    Near near;
    for (auto o : cards) {
      const double lo = horizontal ? o.y : o.x,
                   hi = lo + (horizontal ? o.height : o.width);
      if (v > lo - kClearance && v < hi + kClearance) near.cards.push_back(o);
    }
    for (auto w : wires) {
      const double lo = horizontal ? w->box.y : w->box.x,
                   hi = lo + (horizontal ? w->box.height : w->box.width);
      if (v >= lo - kLane && v <= hi + kLane) near.wires.push_back(w);
    }
    return near;
  }
  static auto run_cost(double u0, double u1, double v, bool horizontal,
                       const Near& near) -> double {
    double cost = u1 - u0;
    for (auto o : near.cards) {
      const double lo = horizontal ? o.x : o.y,
                   hi = lo + (horizontal ? o.width : o.height);
      const double vlo = horizontal ? o.y : o.x,
                   vhi = vlo + (horizontal ? o.height : o.width);
      if (u0 < hi && u1 > lo && v > vlo && v < vhi) return kInfinity;
      cost += kNear * std::max(0.0, std::min(u1, hi + kClearance) -
                                        std::max(u0, lo - kClearance));
    }
    for (auto w : near.wires) {
      const double lo = horizontal ? w->box.x : w->box.y,
                   hi = lo + (horizontal ? w->box.width : w->box.height);
      if (u1 < lo || u0 > hi) continue;
      for (size_t i = 1; i < w->pts.size(); ++i) {
        const auto s = w->pts[i - 1], t = w->pts[i];
        const double su = horizontal ? s.x : s.y, sv = horizontal ? s.y : s.x,
                     tu = horizontal ? t.x : t.y, tv = horizontal ? t.y : t.x;
        if ((sv <= v) != (tv <= v)) {
          const double at = su + ((v - sv) * (tu - su)) / (tv - sv);
          if (at >= u0 && at < u1) cost += kCrossing;
        } else if (std::abs(sv - v) < kLane / 2 &&
                   std::abs(tv - v) < kLane / 2) {
          cost += kOverlap * std::max(0.0, std::min(u1, std::max(su, tu)) -
                                               std::max(u0, std::min(su, tu)));
        }
      }
    }
    return cost;
  }
  auto search(Point a, Point b) -> std::vector<Point> {
    std::vector<Rect> obstacles;
    for (auto o : frame_)
      if (depth(o, a, 1) <= kPortDepth && depth(o, b, -1) <= kPortDepth)
        obstacles.push_back(o);
    const auto out = clear(a, 1, obstacles), into = clear(b, -1, obstacles);
    const double along = into.y - out.y;
    const Point first{out.x, out.y + stub(out, 1, along, obstacles)},
        last{into.x, into.y - stub(into, -1, along, obstacles)};
    std::vector<double> xs, ys, xe, ye;
    for (auto o : frame_) {
      xs.insert(xs.end(), {o.x - kClearance, o.x + o.width + kClearance});
      ys.insert(ys.end(), {o.y - kClearance, o.y + o.height + kClearance});
      xe.insert(xe.end(), {o.x, o.x + o.width});
      ye.insert(ye.end(), {o.y, o.y + o.height});
    }
    auto gaps = [](std::vector<double> edges, std::vector<double>& lines) {
      std::sort(edges.begin(), edges.end());
      for (size_t i = 1; i < edges.size(); ++i)
        if (edges[i] > edges[i - 1] && edges[i] - edges[i - 1] < 2 * kClearance)
          lines.push_back((edges[i] + edges[i - 1]) / 2);
    };
    gaps(xe, xs);
    gaps(ye, ys);
    xs.insert(xs.end(), {first.x, last.x, (first.x + last.x) / 2});
    ys.insert(ys.end(), {first.y, last.y, (first.y + last.y) / 2});
    std::vector<const Run*> others;
    // The reference compares the world-space source to a frame-space point.
    // Preserve that behavior for horizontal flow as well.
    for (const auto& w : routed_)
      if (!equal(w.a, a)) {
        others.push_back(&w);
        if (w.curved) continue;
        for (size_t i = 1; i < w.pts.size(); ++i) {
          auto p = w.pts[i];
          if (p.x == w.pts[i - 1].x)
            xs.insert(xs.end(), {p.x - kLane, p.x + kLane});
          else
            ys.insert(ys.end(), {p.y - kLane, p.y + kLane});
        }
      }
    xs = sorted_unique(std::move(xs));
    ys = sorted_unique(std::move(ys));
    const int nx = static_cast<int>(xs.size()),
              ny = static_cast<int>(ys.size());
    auto index = [](const std::vector<double>& vs, double v) {
      return static_cast<int>(std::find(vs.begin(), vs.end(), v) - vs.begin());
    };
    const int start = index(xs, first.x) + index(ys, first.y) * nx,
              goal = index(xs, last.x) + index(ys, last.y) * nx;
    std::vector<double> best(static_cast<size_t>(nx) * ny * 4, kInfinity),
        stretch(static_cast<size_t>(nx) * ny * 2, -1);
    std::vector<int> from(best.size(), -1);
    std::vector<std::optional<Near>> near_y(ny), near_x(nx);
    auto cost = [&](int n, bool horizontal) {
      const int k = n * 2 + (horizontal ? 1 : 0), ix = n % nx, iy = n / nx;
      if (stretch[k] < 0) {
        auto& near = horizontal ? near_y[iy] : near_x[ix];
        const double v = horizontal ? ys[iy] : xs[ix];
        if (!near) near = near_line(v, horizontal, obstacles, others);
        stretch[k] = horizontal ? run_cost(xs[ix], xs[ix + 1], v, true, *near)
                                : run_cost(ys[iy], ys[iy + 1], v, false, *near);
      }
      return stretch[k];
    };
    auto h = [&](int n, int dir) {
      const double dx = std::abs(xs[n % nx] - last.x),
                   dy = std::abs(ys[n / nx] - last.y);
      return dx + dy + ((dir & 1 ? dy : dx) > 0 ? kTurn : 0);
    };
    using Item = std::pair<double, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> open;
    best[start * 4] = 0;
    open.emplace(h(start, 0), start * 4);
    int end = -1;
    while (!open.empty()) {
      const auto [f, id] = open.top();
      open.pop();
      const int n = id >> 2, dir = id & 3, ix = n % nx, iy = n / nx;
      const double g = best[id];
      if (f > g + h(n, dir)) continue;
      if (n == goal) {
        end = id;
        break;
      }
      for (int d = 0; d < 4; ++d) {
        if (d == ((dir + 2) & 3)) continue;
        if (d == 0   ? iy + 1 >= ny
            : d == 1 ? ix + 1 >= nx
            : d == 2 ? iy == 0
                     : ix == 0)
          continue;
        const int m = d == 0   ? n + nx
                      : d == 1 ? n + 1
                      : d == 2 ? n - nx
                               : n - 1;
        double g2 = g + cost(d == 0 || d == 1 ? n : m, d == 1 || d == 3);
        if (g2 == kInfinity) continue;
        if (d != dir) g2 += kTurn;
        if (m == goal) {
          if (d == 2) continue;
          if (d != 0) g2 += kTurn;
        }
        const int next = m * 4 + d;
        if (g2 < best[next]) {
          best[next] = g2;
          from[next] = id;
          open.emplace(g2 + h(m, d), next);
        }
      }
    }
    if (end < 0) return {};
    std::vector<Point> corners{b, into, last};
    for (int id = from[end]; id >= 0; id = from[id]) {
      const int n = id >> 2;
      corners.push_back({xs[n % nx], ys[n / nx]});
    }
    corners.insert(corners.end(), {first, out, a});
    std::reverse(corners.begin(), corners.end());
    return straightened(corners);
  }
  bool vertical_;
  const std::vector<Rect>& obstacles_;
  std::vector<Rect> frame_;
  std::vector<Run> routed_;
};
}  // namespace

auto route_wires(bool top_to_bottom, const std::vector<Rect>& obstacles,
                 const std::vector<WireEnds>& wires)
    -> std::vector<std::vector<Point>> {
  return Router(top_to_bottom, obstacles).route(wires);
}
auto crossing_pairs(const std::vector<std::vector<Point>>& paths,
                    const std::vector<WireEnds>& ends) -> int {
  std::vector<Rect> boxes;
  for (const auto& ps : paths) boxes.push_back(bounds(ps));
  auto hit = [](Point p, Point q, Point r, Point s) {
    const double dx = q.x - p.x, dy = q.y - p.y, ex = s.x - r.x, ey = s.y - r.y,
                 d = dx * ey - dy * ex;
    if (std::abs(d) < 1e-9) return false;
    const double t = ((r.x - p.x) * ey - (r.y - p.y) * ex) / d,
                 u = ((r.x - p.x) * dy - (r.y - p.y) * dx) / d;
    return t >= -1e-9 && t < 1 - 1e-9 && u >= -1e-9 && u < 1 - 1e-9;
  };
  int count = 0;
  for (size_t i = 0; i < paths.size(); ++i)
    for (size_t j = i + 1; j < paths.size(); ++j) {
      if (equal(ends[i].a, ends[j].a) || equal(ends[i].b, ends[j].b)) continue;
      const auto a = boxes[i], b = boxes[j];
      if (a.x + a.width < b.x || b.x + b.width < a.x || a.y + a.height < b.y ||
          b.y + b.height < a.y)
        continue;
      bool crossed = false;
      for (size_t x = 1; x < paths[i].size() && !crossed; ++x)
        for (size_t y = 1; y < paths[j].size(); ++y) {
          if (hit(paths[i][x - 1], paths[i][x], paths[j][y - 1], paths[j][y])) {
            crossed = true;
            break;
          }
        }
      if (crossed) ++count;
    }
  return count;
}
}  // namespace tsugumi::layout
