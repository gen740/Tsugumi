#include "host/layout.hh"

#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <iterator>
#include <map>

#include "tsugumi/util/json.hh"

namespace tsugumi::layout {
namespace {
using Json = hosq::json::GenericDom<>;
template <class T>
auto decode(const Json& j) -> T {
  T v;
  if (auto e = schema::decode(j, v)) throw std::runtime_error(e->message);
  return v;
}
auto object(const Json& j) -> std::map<std::string, Json> {
  const auto v = decode<schema::v::Mapping<schema::v::String, schema::Json>>(j);
  return {v.value().begin(), v.value().end()};
}
auto array(const Json& j) -> std::vector<Json> {
  return decode<schema::v::Array<schema::Json>>(j).value();
}
auto text(const Json& j) -> std::string {
  return decode<schema::v::String>(j).value();
}
auto number(const Json& j) -> double {
  return decode<schema::Number>(j).value();
}
auto list(const std::map<std::string, Json>& o, const std::string& key)
    -> std::vector<Json> {
  return o.contains(key) ? array(o.at(key)) : std::vector<Json>{};
}
auto fixture(const char* name) -> std::vector<Json> {
  std::ifstream f(std::string(TSUGUMI_LAYOUT_FIXTURES) + "/" + name);
  if (!f) throw std::runtime_error(std::string("missing fixture: ") + name);
  return array(
      schema::capture(std::string(std::istreambuf_iterator<char>(f), {})));
}
TEST(Layout, RouterMatchesPresentationPaths) {
  for (const auto& j : fixture("wire-routes.json")) {
    const auto c = object(j);
    SCOPED_TRACE(text(c.at("name")));
    std::vector<Rect> obstacles;
    for (const auto& o : list(c, "obstacles")) {
      const auto r = object(o);
      obstacles.push_back({number(r.at("x")), number(r.at("y")),
                           number(r.at("width")), number(r.at("height"))});
    }
    auto point = [](const Json& j) {
      const auto p = object(j);
      return Point{number(p.at("x")), number(p.at("y"))};
    };
    std::vector<WireEnds> wires;
    for (const auto& j : list(c, "wires")) {
      const auto w = object(j);
      wires.push_back({point(w.at("a")), point(w.at("b"))});
    }
    const auto paths =
        route_wires(text(c.at("flow")) == "topToBottom", obstacles, wires);
    ASSERT_EQ(paths.size(), wires.size());
    for (size_t i = 0; i < paths.size(); ++i) {
      const auto& ps = paths[i];
      ASSERT_FALSE(ps.empty());
      EXPECT_NEAR(ps.front().x, wires[i].a.x, 1e-6);
      EXPECT_NEAR(ps.front().y, wires[i].a.y, 1e-6);
      EXPECT_NEAR(ps.back().x, wires[i].b.x, 1e-6);
      EXPECT_NEAR(ps.back().y, wires[i].b.y, 1e-6);
      for (const auto p : ps)
        EXPECT_TRUE(std::isfinite(p.x) && std::isfinite(p.y));
      for (const auto& box : obstacles) {
        const auto inside = [&](auto p) {
          return p.x > box.x + 2 && p.x < box.x + box.width - 2 &&
                 p.y > box.y + 2 && p.y < box.y + box.height - 2;
        };
        if (inside(ps.front()) || inside(ps.back())) continue;
        for (size_t k = 1; k < ps.size(); ++k)
          for (int sample = 0; sample <= 20; ++sample) {
            const auto t = sample / 20.0;
            EXPECT_FALSE(
                inside(Point{ps[k - 1].x + t * (ps[k].x - ps[k - 1].x),
                             ps[k - 1].y + t * (ps[k].y - ps[k - 1].y)}));
          }
      }
    }
  }
}

// A junction sits in the free lane below the output, across the flow at the
// median of its inputs, and clear of the Cards.
TEST(Layout, JunctionGoesToTheLaneAtTheMedianOfItsInputs) {
  Scene scene;
  auto card = [&](const std::string& id, double x, double y, double height) {
    Card c;
    c.id = id;
    c.x = x;
    c.y = y;
    c.width = 160;
    c.height = height;
    c.inputs.push_back({"in", 80, 0});
    c.outputs.push_back({"out", 80, height});
    scene.cards.push_back(c);
  };
  card("s", 0, 0, 120);
  // Beside the source and taller: the lane starts below it.
  card("tall", 200, 0, 200);
  for (const auto* id : {"a", "b", "c"}) {
    card(id, (id[0] - 'a') * 200.0, 400, 120);
    scene.wires.push_back({"s", "out", id, "in"});
  }
  auto only = [&] {
    const auto list = junctions(scene, {});
    EXPECT_EQ(list.size(), 1U);
    return list.empty() ? Junction{} : list.front();
  };
  const auto j = only();
  EXPECT_DOUBLE_EQ(j.y, 300);  // the middle of 200..400
  EXPECT_DOUBLE_EQ(j.x, 280);  // the median of 80, 280, 480

  // A Card in the lane where the median falls: to its nearer side, half a
  // grid clear of it.
  card("m", 220, 250, 100);
  const auto clear = only();
  EXPECT_DOUBLE_EQ(clear.y, 300);
  EXPECT_DOUBLE_EQ(clear.x, 210);
}
}  // namespace
}  // namespace tsugumi::layout
