#include "host/layout.hh"

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
