#include "layout.hh"

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <map>

#include "schema.hh"

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
auto strings(const std::vector<Json>& a) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (const auto& x : a) out.push_back(text(x));
  return out;
}
auto ports(const Json& j) -> std::vector<Port> {
  std::vector<Port> out;
  for (const auto& x : array(j)) {
    auto o = object(x);
    out.push_back({text(o.at("name")), number(o.at("x")), number(o.at("y"))});
  }
  return out;
}
auto bar(const std::map<std::string, Json>& o, const std::string& side) -> Bar {
  Bar out;
  if (!o.contains(side)) return out;
  const auto b = object(o.at(side));
  for (const auto& x : list(b, "ends")) {
    auto e = object(x);
    out.ends.push_back(
        {text(e.at("name")), text(e.at("card")), text(e.at("port"))});
  }
  if (b.contains("at")) {
    auto p = object(b.at("at"));
    out.at = Point{number(p.at("x")), number(p.at("y"))};
  }
  return out;
}
auto scene(const Json& j) -> Scene {
  const auto o = object(j);
  Scene s;
  s.top_to_bottom = text(o.at("flow")) == "topToBottom";
  s.grid = number(o.at("grid"));
  s.snap = decode<schema::v::Bool>(o.at("snap")).value();
  for (const auto& x : list(o, "cards")) {
    const auto c = object(x);
    s.cards.push_back({text(c.at("id")), number(c.at("x")), number(c.at("y")),
                       number(c.at("width")), number(c.at("height")),
                       ports(c.at("inputs")), ports(c.at("outputs")),
                       decode<schema::v::Bool>(c.at("reorderable")).value()});
  }
  for (const auto& x : list(o, "wires")) {
    auto w = object(x);
    s.wires.push_back({text(w.at("from")), text(w.at("fromPort")),
                       text(w.at("to")), text(w.at("toPort"))});
  }
  for (const auto& side : {"frames", "notes"})
    for (const auto& x : list(o, side)) {
      auto b = object(x);
      (std::string(side) == "frames" ? s.frames : s.notes)
          .push_back({text(b.at("id")), number(b.at("x")), number(b.at("y")),
                      number(b.at("width")), number(b.at("height"))});
    }
  s.inputs = bar(o, "inputs");
  s.outputs = bar(o, "outputs");
  return s;
}
auto check_places(const std::vector<Place>& got, const Json& j) -> void {
  const auto expected = array(j);
  ASSERT_EQ(got.size(), expected.size());
  for (size_t i = 0; i < got.size(); ++i) {
    const auto p = object(expected[i]);
    EXPECT_EQ(got[i].id, text(p.at("id")));
    // Exact double equality, deliberately not EXPECT_DOUBLE_EQ's ULP tolerance.
    EXPECT_EQ(got[i].x, number(p.at("x")));
    EXPECT_EQ(got[i].y, number(p.at("y")));
  }
}
auto fixture(const char* name) -> std::vector<Json> {
  std::ifstream f(std::string(TSUGUMI_LAYOUT_FIXTURES) + "/" + name);
  if (!f) throw std::runtime_error(std::string("missing fixture: ") + name);
  return array(
      schema::capture(std::string(std::istreambuf_iterator<char>(f), {})));
}
// Recorded by web/tests/layoutGolden.ts at 9c127594 from the original TS
// implementation, before its removal. Includes demos, groups, nested Cards,
// feedback, both flows, grids on/off, and drag neighborhoods. Never regenerate
// expected values from the C++ implementation.
TEST(Layout, MatchesTypeScriptGoldenExactly) {
  const auto cases = fixture("layout-golden.json");
  ASSERT_FALSE(cases.empty());
  for (const auto& j : cases) {
    const auto c = object(j);
    SCOPED_TRACE(text(c.at("name")));
    const auto s = scene(c.at("scene"));
    const auto got = text(c.at("request")) == "arrange"
                         ? arrange(s)
                         : untangle(s, strings(list(c, "around")));
    const auto expected = object(c.at("expect"));
    check_places(got.cards, expected.at("cards"));
    check_places(got.frames, expected.at("frames"));
    check_places(got.notes, expected.at("notes"));
    const auto os = list(expected, "portOrders");
    ASSERT_EQ(got.port_orders.size(), os.size());
    for (size_t i = 0; i < os.size(); ++i) {
      const auto o = object(os[i]);
      EXPECT_EQ(got.port_orders[i].id, text(o.at("id")));
      EXPECT_EQ(got.port_orders[i].inputs, strings(list(o, "inputs")));
      EXPECT_EQ(got.port_orders[i].outputs, strings(list(o, "outputs")));
    }
  }
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
    const auto expected = list(c, "paths");
    ASSERT_EQ(paths.size(), expected.size());
    for (size_t i = 0; i < paths.size(); ++i) {
      // null denotes a sampled curve in the original presentation contract.
      if (schema::is_null(expected[i])) continue;
      const auto ps = array(expected[i]);
      ASSERT_EQ(paths[i].size(), ps.size());
      for (size_t k = 0; k < ps.size(); ++k) {
        EXPECT_EQ(paths[i][k].x, point(ps[k]).x);
        EXPECT_EQ(paths[i][k].y, point(ps[k]).y);
      }
    }
  }
}
}  // namespace
}  // namespace tsugumi::layout
