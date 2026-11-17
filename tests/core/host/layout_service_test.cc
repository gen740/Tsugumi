#include "host/layout_service.hh"

#include <gtest/gtest.h>

#include <cmath>
#include <functional>
#include <limits>
#include <memory>

#include "agent/catalog.hh"
#include "tsugumi/util/json_object.hh"
#include "host/layout_saving.hh"
#include "tsugumi/util/fs.hh"

namespace tsugumi::host {
namespace {
namespace H = Tsugumi::Host;

TEST(HostLayout, ValidationRejectsTheOpaqueCoreVectors) {
  const auto cases = ::tsugumi::json::Array(fs::read_file_or_throw(
      std::string(TSUGUMI_LAYOUT_FIXTURES) + "/layout-invalid.json"));
  ASSERT_EQ(cases.size(), 16u);
  for (const auto& raw : cases) {
    const auto value = ::tsugumi::json::Object(raw);
    SCOPED_TRACE(::tsugumi::json::Text(value, "name"));
    flatbuffers::FlatBufferBuilder b;
    const auto input = H::CreateLayoutValidateRequest(
        b, b.CreateString(::tsugumi::json::Text(value, "json")));
    b.Finish(H::CreateRequest(b, H::RequestPayload::LayoutValidateRequest,
                              input.Union()));
    EXPECT_THROW(
        LayoutHandle(*flatbuffers::GetRoot<H::Request>(b.GetBufferPointer()),
                     7),
        LayoutFailure);
  }
}

auto SceneFixture(const ::tsugumi::json::Members &value) -> H::LayoutSceneT {
  namespace json = ::tsugumi::json;
  H::LayoutSceneT scene;
  scene.flow = json::Text(value, "flow");
  scene.grid = std::stod(json::Get(value, "grid", "0"));
  scene.snap = json::Bool(json::Get(value, "snap", "false"));
  auto coordinate = [&](const auto &fields, const auto &key) {
    return std::stod(json::Get(fields, key, "0"));
  };
  auto ports = [&](const std::string &raw, auto &result) {
    for (const auto &item : json::Array(raw)) {
      const auto fields = json::Object(item);
      auto port = std::make_unique<H::LayoutPortT>();
      port->name = json::Text(fields, "name");
      port->x = coordinate(fields, "x");
      port->y = coordinate(fields, "y");
      result.push_back(std::move(port));
    }
  };
  for (const auto &raw : json::Array(json::Get(value, "cards", "[]"))) {
    const auto fields = json::Object(raw);
    auto card = std::make_unique<H::LayoutCardT>();
    card->id = json::Text(fields, "id");
    card->x = coordinate(fields, "x");
    card->y = coordinate(fields, "y");
    card->width = coordinate(fields, "width");
    card->height = coordinate(fields, "height");
    card->reorderable = json::Bool(json::Get(fields, "reorderable", "false"));
    ports(json::Get(fields, "inputs", "[]"), card->inputs);
    ports(json::Get(fields, "outputs", "[]"), card->outputs);
    scene.cards.push_back(std::move(card));
  }
  for (const auto &raw : json::Array(json::Get(value, "wires", "[]"))) {
    const auto fields = json::Object(raw);
    auto wire = std::make_unique<H::LayoutWireT>();
    wire->from = json::Text(fields, "from");
    wire->from_port = json::Text(fields, "fromPort");
    wire->to = json::Text(fields, "to");
    wire->to_port = json::Text(fields, "toPort");
    scene.wires.push_back(std::move(wire));
  }
  auto boxes = [&](const auto &field, auto &result) {
    for (const auto &raw : json::Array(json::Get(value, field, "[]"))) {
      const auto fields = json::Object(raw);
      auto box = std::make_unique<H::LayoutBoxT>();
      box->id = json::Text(fields, "id");
      box->x = coordinate(fields, "x");
      box->y = coordinate(fields, "y");
      box->width = coordinate(fields, "width");
      box->height = coordinate(fields, "height");
      result.push_back(std::move(box));
    }
  };
  boxes("frames", scene.frames);
  boxes("notes", scene.notes);
  auto bar = [&](const auto &side, auto &result) {
    if (!value.contains(side))
      return;
    result = std::make_unique<H::LayoutBarT>();
    const auto fields = json::Object(value.at(side));
    if (fields.contains("at")) {
      const auto at = json::Object(fields.at("at"));
      result->x = coordinate(at, "x");
      result->y = coordinate(at, "y");
    }
    for (const auto &raw : json::Array(json::Get(fields, "ends", "[]"))) {
      const auto end = json::Object(raw);
      auto item = std::make_unique<H::LayoutBarEndT>();
      item->name = json::Text(end, "name");
      item->card = json::Text(end, "card");
      item->port = json::Text(end, "port");
      result->ends.push_back(std::move(item));
    }
  };
  bar("inputs", scene.inputs);
  bar("outputs", scene.outputs);
  return scene;
}
TEST(HostLayout, GoldenScenesRetainTheirSharedOperationGeometry) {
  namespace json = ::tsugumi::json;
  for (const auto &raw : json::Array(fs::read_file_or_throw(
           std::string(TSUGUMI_LAYOUT_FIXTURES) + "/layout-golden.json"))) {
    const auto value = json::Object(raw);
    SCOPED_TRACE(json::Text(value, "name"));
    const auto scene =
        MeasuredScene(SceneFixture(json::Object(value.at("scene"))));
    std::vector<std::string> around;
    for (const auto &id : json::Array(json::Get(value, "around", "[]")))
      around.push_back(json::String(id));
    const auto proposal = json::Text(value, "request") == "arrange"
                              ? layout::arrange(scene)
                              : layout::untangle(scene, around);
    const auto expected = json::Object(value.at("expect"));
    auto places = [&](const auto &got, const auto &field) {
      const auto wanted = json::Array(expected.at(field));
      ASSERT_EQ(got.size(), wanted.size());
      for (size_t i = 0; i < got.size(); ++i) {
        const auto item = json::Object(wanted[i]);
        EXPECT_EQ(got[i].id, json::Text(item, "id"));
        EXPECT_DOUBLE_EQ(got[i].x, std::stod(item.at("x")));
        EXPECT_DOUBLE_EQ(got[i].y, std::stod(item.at("y")));
      }
    };
    places(proposal.cards, "cards");
    places(proposal.frames, "frames");
    places(proposal.notes, "notes");
    const auto orders = json::Array(expected.at("portOrders"));
    ASSERT_EQ(proposal.port_orders.size(), orders.size());
    for (size_t i = 0; i < orders.size(); ++i) {
      const auto order = json::Object(orders[i]);
      EXPECT_EQ(proposal.port_orders[i].id, json::Text(order, "id"));
      auto names = [&](const auto &side) {
        std::vector<std::string> result;
        for (const auto &port : json::Array(order.at(side)))
          result.push_back(json::String(port));
        return result;
      };
      EXPECT_EQ(proposal.port_orders[i].inputs, names("inputs"));
      EXPECT_EQ(proposal.port_orders[i].outputs, names("outputs"));
    }
  }
}

TEST(HostLayout, OmittedVersionCanvasVectorsAreEmpty) {
  flatbuffers::FlatBufferBuilder b;
  b.Finish(H::CreateVersionCanvasRequest(b, b.CreateString("{}")));
  const auto result = VersionCanvas(
      *flatbuffers::GetRoot<H::VersionCanvasRequest>(b.GetBufferPointer()));
  EXPECT_TRUE(result.cards.empty());
  EXPECT_TRUE(result.wires.empty());
  EXPECT_EQ(result.flow, "leftToRight");
}

TEST(HostLayout, SchemaRejectsIncompleteAndUnknownPresentationFields) {
  EXPECT_EQ(ValidateLayout(" { } "), " { } ");
  for (const auto* json :
       {R"({"cards":{"a":{"x":1}}})", R"({"notes":[{"id":"n","x":1,"y":2}]})",
        R"({"displays":{"a":{"x":1,"y":2}}})", R"({"groups":[{"typo":true}]})",
        "[]"})
    EXPECT_THROW(ValidateLayout(json), LayoutFailure) << json;
}

TEST(HostLayout, SavingClientRemovesOnlyDeletedCardReferences) {
  const auto text =
      R"({"cards":{"a":{"x":1,"y":2},"a/child":{"x":3,"y":4},"ab":{"x":5,"y":6}},"displays":{"a":{"x":1,"y":2,"width":3,"height":4}},"boundaries":{"a":{"inputs":{"x":1,"y":2}}},"groups":[{"cards":["a","ab"],"outputs":[{"card":"a","port":"x"},{"card":"ab","port":"y"}]}]})";
  const auto result = ::tsugumi::json::Object(ValidateLayout(text, {"a"}));
  EXPECT_EQ(::tsugumi::json::Object(result.at("cards")).size(), 1u);
  EXPECT_TRUE(::tsugumi::json::Object(result.at("cards")).contains("ab"));
  EXPECT_TRUE(::tsugumi::json::Object(result.at("displays")).empty());
  EXPECT_TRUE(::tsugumi::json::Object(result.at("boundaries")).empty());
  const auto group =
      ::tsugumi::json::Object(::tsugumi::json::Array(result.at("groups")).at(0));
  EXPECT_EQ(::tsugumi::json::Array(group.at("cards")).size(), 1u);
  EXPECT_EQ(::tsugumi::json::Array(group.at("outputs")).size(), 1u);
}

auto canvas(const std::string& layout) -> H::VersionCanvasT {
  flatbuffers::FlatBufferBuilder b;
  auto a = H::CreateVersionCanvasCard(b, b.CreateString("a"), 0, 0,
                                      b.CreateString("input"));
  auto c = H::CreateVersionCanvasCard(b, b.CreateString("b"), 0, 0,
                                      b.CreateString("fork"), 3);
  auto w =
      H::CreateVersionCanvasWire(b, b.CreateString("a"), b.CreateString("b"));
  auto req = H::CreateVersionCanvasRequest(b, b.CreateString(layout),
                                           b.CreateVector(std::vector{a, c}),
                                           b.CreateVector(std::vector{w}));
  b.Finish(req);
  return VersionCanvas(
      *flatbuffers::GetRoot<H::VersionCanvasRequest>(b.GetBufferPointer()));
}

TEST(HostLayout, VersionCanvasGuessesFlowAndReadsCollapsedState) {
  auto result = canvas(
      R"({"cards":{"a":{"x":0,"y":0},"b":{"x":0,"y":100,"collapsed":true}}})");
  EXPECT_EQ(result.flow, "topToBottom");
  EXPECT_TRUE(result.cards.at(1)->collapsed);
  EXPECT_EQ(result.cards.at(1)->fan, 3u);
  result = canvas(
      R"({"flow":"leftToRight","cards":{"a":{"x":0,"y":0},"b":{"x":0,"y":100}}})");
  EXPECT_EQ(result.flow, "leftToRight");
}

TEST(HostLayout, VersionCanvasFallsBackForMissingOrInvalidLayout) {
  for (const auto* layout :
       {"{}", R"({"cards":{"a":{"x":7,"y":9}}})", R"({"future":true})"}) {
    const auto result = canvas(layout);
    EXPECT_EQ(result.flow, "leftToRight");
    EXPECT_LT(result.cards.at(0)->x, result.cards.at(1)->x);
  }
}

TEST(HostLayout, AgentLayoutAppliesAnOperationAndEditorsKeepPureProposals) {
  const agent::Catalog catalog;
  ASSERT_NE(catalog.Find("project_save"), nullptr);
  const auto* tool = catalog.Find("project_layout");
  ASSERT_NE(tool, nullptr);
  EXPECT_TRUE(tool->mutating);
  EXPECT_EQ(catalog.Find("layout_arrange"), nullptr);
  EXPECT_EQ(catalog.Find("layout_untangle"), nullptr);
  const auto properties = ::tsugumi::json::Object(::tsugumi::json::Get(
      ::tsugumi::json::Object(tool->spec.input_schema), "properties"));
  EXPECT_TRUE(properties.contains("action"));
  EXPECT_FALSE(properties.contains("scene"));
  EXPECT_THROW(agent::EncodeProjectRequest("project_layout", "{}", 1),
               std::runtime_error);
}
}  // namespace
}  // namespace tsugumi::host

namespace tsugumi::host {
namespace {
TEST(HostLayoutSaving, RootCleanupIsPartOfTheSameSave) {
  const auto args =
      R"({"base_rev":"7","changes":[{"value_type":"SetProject","value":{"json":"{\"canvas\":{\"cards\":[]}}"}}]})";
  const auto saved = ::tsugumi::json::Object(PrepareLayoutSave(
      args, [](const std::string& path) -> std::optional<LayoutFile> {
        if (path == "tsugumi.json")
          return LayoutFile{R"({"canvas":{"cards":[{"id":"a"}]}})", {}};
        if (path == "layout.json")
          return LayoutFile{R"({"cards":{"a":{"x":1,"y":2}}})", {}};
        return std::nullopt;
      }));
  EXPECT_EQ(::tsugumi::json::Text(saved, "base_rev"), "7");
  const auto changes = ::tsugumi::json::Array(saved.at("changes"));
  ASSERT_EQ(changes.size(), 2u);
  const auto cleanup = ::tsugumi::json::Object(changes.at(1));
  EXPECT_EQ(::tsugumi::json::Text(cleanup, "value_type"), "SetLayout");
  const auto layout = ::tsugumi::json::Object(
      ::tsugumi::json::Text(::tsugumi::json::Object(cleanup.at("value")), "json"));
  EXPECT_TRUE(::tsugumi::json::Object(layout.at("cards")).empty());
}

TEST(HostLayoutSaving, ExternalCleanupRetainsSourceHashGuard) {
  const auto args =
      R"({"changes":[{"value_type":"PutSource","value":{"path":"externals/child/tsugumi.json","bytes":"{\"canvas\":{\"cards\":[]}}","base_hash":"project"}}]})";
  const auto saved = ::tsugumi::json::Object(PrepareLayoutSave(
      args, [](const std::string& path) -> std::optional<LayoutFile> {
        if (path == "externals/child/tsugumi.json")
          return LayoutFile{R"({"canvas":{"cards":[{"id":"a"}]}})", "project"};
        if (path == "externals/child/layout.json")
          return LayoutFile{R"({"cards":{"a":{"x":1,"y":2}}})", "layout"};
        return std::nullopt;
      }));
  const auto changes = ::tsugumi::json::Array(saved.at("changes"));
  ASSERT_EQ(changes.size(), 2u);
  const auto cleanup = ::tsugumi::json::Object(
      ::tsugumi::json::Get(::tsugumi::json::Object(changes.at(1)), "value"));
  EXPECT_EQ(::tsugumi::json::Text(cleanup, "path"), "externals/child/layout.json");
  EXPECT_EQ(::tsugumi::json::Text(cleanup, "base_hash"), "layout");
  EXPECT_TRUE(::tsugumi::json::Object(
                  ::tsugumi::json::Get(
                      ::tsugumi::json::Object(::tsugumi::json::Text(cleanup, "bytes")),
                      "cards"))
                  .empty());
}

TEST(HostLayoutSaving, PatchesAreValidatedAndSourceLookalikesAreUnchanged) {
  const auto read = [](const std::string&) -> std::optional<LayoutFile> {
    return LayoutFile{"{}", "hash"};
  };
  const auto invalid =
      R"({"changes":[{"value_type":"PatchSource","value":{"path":"externals/child/layout.json","base_hash":"hash","patches":[{"offset":"1","remove":"0","text":"\"cards\":1"}]}}]})";
  EXPECT_THROW(PrepareLayoutSave(invalid, read), LayoutFailure);
  const auto source =
      R"({"changes":[{"value_type":"PutSource","value":{"path":"definitions/a/src/layout.json","bytes":"not JSON"}}]})";
  EXPECT_NO_THROW(PrepareLayoutSave(source, read));
  EXPECT_TRUE(
      IsExternalDocument("externals/a/externals/b/layout.json", "layout.json"));
  EXPECT_FALSE(IsExternalDocument("externals/a/definitions/b/src/layout.json",
                                  "layout.json"));
  EXPECT_FALSE(IsExternalDocument("layout.json", "layout.json"));
}
}  // namespace
}  // namespace tsugumi::host

namespace tsugumi::host {
namespace {
TEST(HostCardPlacement, ConnectedIsolatedAndBulkCardsGetFreePlaces) {
  namespace json = ::tsugumi::json;
  const std::string original =
      R"({"cards":{"fixed":{"x":123,"y":456,"collapsed":true},"other":{"x":1300,"y":456}}})";
  for (
      const auto* connections :
      {"[]",
       R"([{"from":{"card":"fixed","port":""},"to":{"card":"new","port":""}}])",
       R"([{"from":{"card":"fixed","port":""},"to":{"card":"new","port":""}},{"from":{"card":"other","port":""},"to":{"card":"new","port":""}}])"}) {
    std::vector<std::string> cards{R"({"id":"fixed"})", R"({"id":"other"})",
                                   R"({"id":"new"})"};
    for (int i = 0; i < 12; ++i)
      cards.push_back(json::Encode(
          json::Members{{"id", json::Quote("new" + std::to_string(i))}}));
    const auto project = json::Encode(json::Members{
        {"canvas", json::Encode(json::Members{{"cards", json::Encode(cards)},
                                              {"connections", connections}})}});
    const auto placed = CompleteLayout(project, original);
    const auto positions = json::Object(json::Object(placed).at("cards"));
    EXPECT_EQ(positions.size(), cards.size());
    EXPECT_EQ(positions.at("fixed"),
              json::Object(json::Object(original).at("cards")).at("fixed"));
    EXPECT_EQ(CompleteLayout(project, placed), placed);
    for (const auto& [id, a] : positions)
      for (const auto& [other, b] : positions) {
        if (id >= other) continue;
        const auto ap = json::Object(a), bp = json::Object(b);
        const auto dx = std::abs(std::stod(ap.at("x")) - std::stod(bp.at("x")));
        const auto dy = std::abs(std::stod(ap.at("y")) - std::stod(bp.at("y")));
        EXPECT_TRUE(dx >= 340 || dy >= 260) << id << " overlaps " << other;
      }
  }
}
TEST(HostCardPlacement, APersonsDropAndNestedWorkflowAreKeptInTheTransaction) {
  namespace json = ::tsugumi::json;
  const auto args = json::Encode(json::Members{
      {"changes",
       json::Encode(std::vector<std::string>{
           json::Encode(json::Members{
               {"value_type", json::Quote("PutSource")},
               {"value",
                json::Encode(json::Members{
                    {"path", json::Quote("externals/inner/tsugumi.json")},
                    {"bytes",
                     json::Quote(
                         R"({"canvas":{"cards":[{"id":"dropped"},{"id":"new"}],"connections":[]}})")}})}}),
           json::Encode(json::Members{
               {"value_type", json::Quote("PutSource")},
               {"value",
                json::Encode(json::Members{
                    {"path", json::Quote("externals/inner/layout.json")},
                    {"bytes",
                     json::Quote(
                         R"({"cards":{"dropped":{"x":17,"y":29}}})")}})}})})}});
  const auto saved = json::Object(PrepareLayoutSave(
      args, [](const std::string&) -> std::optional<LayoutFile> {
        return std::nullopt;
      }));
  const auto changes = json::Array(saved.at("changes"));
  ASSERT_EQ(changes.size(), 2u);
  const auto value = json::Object(json::Get(json::Object(changes[1]), "value"));
  const auto positions =
      json::Object(json::Object(json::Text(value, "bytes")).at("cards"));
  EXPECT_EQ(json::Object(positions.at("dropped")).at("x"), "17");
  EXPECT_EQ(json::Object(positions.at("dropped")).at("y"), "29");
  EXPECT_TRUE(positions.contains("new"));
}
}  // namespace
}  // namespace tsugumi::host

namespace tsugumi::host {
namespace {
TEST(HostCardPlacement, PlacesNearItsNeighbourAndAvoidsSavedFramesAndNotes) {
  namespace json = ::tsugumi::json;
  const auto project =
      R"({"canvas":{"cards":[{"id":"far"},{"id":"near"},{"id":"new"}],"connections":[{"from":{"card":"near","port":"out"},"to":{"card":"new","port":"in"}}]}})";
  const auto layout =
      R"({"cards":{"far":{"x":10000,"y":0},"near":{"x":0,"y":0}},"displays":{"near":{"x":340,"y":0,"width":400,"height":300}},"notes":[{"id":"n","text":"Keep me","x":0,"y":300,"width":300,"height":200}]})";
  const auto completed = json::Object(CompleteLayout(project, layout));
  const auto positions = json::Object(completed.at("cards"));
  const auto p = json::Object(positions.at("new"));
  const auto x = std::stod(p.at("x")), y = std::stod(p.at("y"));
  EXPECT_LT(std::abs(x) + std::abs(y), 2000);
  auto disjoint = [&](double bx, double by, double w, double h) {
    return x + 320 <= bx || bx + w <= x || y + 270 <= by || by + h <= y;
  };
  EXPECT_TRUE(disjoint(340, 0, 400, 300));
  EXPECT_TRUE(disjoint(0, 300, 300, 200));
  EXPECT_EQ(completed.at("displays"), json::Object(layout).at("displays"));
  EXPECT_EQ(completed.at("notes"), json::Object(layout).at("notes"));
}
}  // namespace
}  // namespace tsugumi::host

namespace tsugumi::host {
namespace {
TEST(HostLayout, DefaultDisplayGeometryIsServedToBothClients) {
  flatbuffers::FlatBufferBuilder b;
  const auto input = H::CreateDisplayGeometryRequest(b);
  b.Finish(H::CreateRequest(b, H::RequestPayload::DisplayGeometryRequest,
                            input.Union()));
  const auto reply =
      LayoutHandle(*flatbuffers::GetRoot<H::Request>(b.GetBufferPointer()), 9);
  const auto* frames = H::GetMessage(reply.data())
                           ->payload_as_Reply()
                           ->value_as_DisplayGeometries();
  ASSERT_NE(frames, nullptr);
  for (bool vertical : {true, false}) {
    const auto* g =
        vertical ? frames->top_to_bottom() : frames->left_to_right();
    ASSERT_NE(g, nullptr);
    EXPECT_EQ(g->anchor_x(), vertical ? 1 : 0);
    EXPECT_EQ(g->anchor_y(), vertical ? 0 : 1);
    EXPECT_EQ(g->x(), vertical ? 40 : 0);
    EXPECT_EQ(g->y(), vertical ? 0 : 30);
    EXPECT_EQ(g->width(), 320);
    EXPECT_EQ(g->height(), 170);
  }
}

TEST(HostCardPlacement, DefaultDisplayBlocksTheTemptingRightHandSlot) {
  namespace json = ::tsugumi::json;
  // Left and below are occupied. No display position is saved: both UIs
  // nevertheless show the source's output to its right after a display call.
  const auto project =
      R"({"canvas":{"cards":[{"id":"source"},{"id":"left"},{"id":"below"},{"id":"new"}],"connections":[{"from":{"card":"source","port":"out"},"to":{"card":"new","port":"in"}}]}})";
  const auto layout =
      R"({"flow":"topToBottom","cards":{"source":{"x":0,"y":0},"left":{"x":-340,"y":0},"below":{"x":0,"y":300}}})";
  const auto completed = json::Object(CompleteLayout(project, layout));
  const auto positions = json::Object(completed.at("cards"));
  const auto p = json::Object(positions.at("new"));
  const auto x = std::stod(p.at("x")), y = std::stod(p.at("y"));
  // Check expanded and folded source sizes, using the host geometry that the
  // UIs transform by their measured Card dimensions.
  const auto d = DefaultDisplayGeometry(true);
  for (double card_width : {112., 220., 320.}) {
    const auto fx = card_width * d.anchor_x + d.x;
    EXPECT_TRUE(x + 320 <= fx || fx + d.width <= x || y + 270 <= d.y ||
                d.y + d.height <= y)
        << "new Card at " << x << ',' << y << " overlaps the default display";
  }
  EXPECT_FALSE(completed.contains("displays"));
  const auto old = json::Object(json::Object(layout).at("cards"));
  for (const auto& [id, value] : old) EXPECT_EQ(positions.at(id), value);
}
}  // namespace
}  // namespace tsugumi::host

namespace tsugumi::host {
namespace {
TEST(HostCardPlacement, UnsavedFlowReservesEitherDefaultDisplay) {
  namespace json = ::tsugumi::json;
  const auto project =
      R"({"canvas":{"cards":[{"id":"source"},{"id":"new"}],"connections":[{"from":{"card":"source","port":"out"},"to":{"card":"new","port":"in"}}]}})";
  const auto completed = json::Object(
      CompleteLayout(project, R"({"cards":{"source":{"x":0,"y":0}}})"));
  const auto p = json::Object(json::Object(completed.at("cards")).at("new"));
  const auto x = std::stod(p.at("x")), y = std::stod(p.at("y"));
  for (bool vertical : {true, false}) {
    const auto d = DefaultDisplayGeometry(vertical);
    const auto fx = d.anchor_x * 220 + d.x, fy = d.anchor_y * 170 + d.y;
    EXPECT_TRUE(x + 320 <= fx || fx + d.width <= x || y + 270 <= fy ||
                fy + d.height <= y);
  }
}
}  // namespace
}  // namespace tsugumi::host

namespace tsugumi::host {
namespace {
TEST(HostLayoutSaving, AnySaveMaterializesDerivedPlacesAtEveryDepth) {
  namespace json = ::tsugumi::json;
  const std::string project =
      R"({"canvas":{"cards":[{"id":"a"}],"connections":[]}})";
  const std::map<std::string, LayoutFile> files{
      {"tsugumi.json", {project, {}}},
      {"layout.json", {"{}", {}}},
      {"externals/one/externals/two/tsugumi.json", {project, "project-hash"}}};
  const auto read = [&](const std::string& path) -> std::optional<LayoutFile> {
    const auto found = files.find(path);
    return found == files.end() ? std::nullopt : std::optional{found->second};
  };
  const auto saved = json::Object(PrepareLayoutSave(
      R"({"base_rev":"7","changes":[{"value_type":"PutSource","value":{"path":"definitions/a/code.py","bytes":"source"}}]})",
      read, {"tsugumi.json", "externals/one/externals/two/tsugumi.json"}));
  EXPECT_EQ(json::Text(saved, "base_rev"), "7");
  const auto changes = json::Array(saved.at("changes"));
  ASSERT_EQ(changes.size(), 3u);
  for (size_t i = 1; i < changes.size(); ++i) {
    const auto change = json::Object(changes[i]);
    const auto value = json::Object(change.at("value"));
    const bool root = json::Text(change, "value_type") == "SetLayout";
    EXPECT_EQ(json::Text(value, root ? "json" : "bytes"),
              CompleteLayout(project, "{}"));
    if (!root)
      EXPECT_EQ(json::Text(value, "path"),
                "externals/one/externals/two/layout.json");
  }
}

TEST(HostLayoutSaving, AddingAndPatchingKeepThePlacesAlreadyDerivedForDisplay) {
  namespace json = ::tsugumi::json;
  const std::string before =
      R"({"canvas":{"cards":[{"id":"legacy"}],"connections":[]}})";
  const std::string after =
      R"({"canvas":{"cards":[{"id":"new"},{"id":"legacy"}],"connections":[]}})";
  const auto read = [&](const std::string& path) -> std::optional<LayoutFile> {
    if (path == "tsugumi.json") return LayoutFile{before, {}};
    return std::nullopt;
  };
  const auto saved = json::Object(PrepareLayoutSave(
      json::Encode(json::Members{
          {"changes",
           json::Encode(std::vector<std::string>{json::Encode(json::Members{
               {"value_type", json::Quote("SetProject")},
               {"value", json::Encode(json::Members{
                             {"json", json::Quote(after)}})}})})}}),
      read));
  const auto layout = json::Object(json::Text(
      json::Object(
          json::Object(json::Array(saved.at("changes"))[1]).at("value")),
      "json"));
  const auto positions = json::Object(layout.at("cards"));
  EXPECT_EQ(positions.at("legacy"),
            json::Object(json::Object(CompleteLayout(before, "{}")).at("cards"))
                .at("legacy"));
  EXPECT_NE(positions.at("new"), positions.at("legacy"));
}

TEST(HostCardPlacement, DisplayRpcUsesTheSameDefinitionsAndGeometryAsSaving) {
  namespace json = ::tsugumi::json;
  const std::string project =
      R"({"canvas":{"cards":[{"id":"wide"},{"id":"new"}],"connections":[]}})";
  const std::string definition =
      R"({"params":[{},{},{}],"inputs":[{},{},{},{},{},{},{},{}]})";
  flatbuffers::FlatBufferBuilder builder;
  const auto input = H::CreateLayoutCompleteRequest(
      builder, builder.CreateString(project), builder.CreateString("{}"),
      builder.CreateString(
          json::Encode(json::Members{{"wide", json::Quote(definition)}})));
  builder.Finish(H::CreateRequest(
      builder, H::RequestPayload::LayoutCompleteRequest, input.Union()));
  const auto result = LayoutHandle(
      *flatbuffers::GetRoot<H::Request>(builder.GetBufferPointer()), 7);
  EXPECT_EQ(
      H::GetMessage(result.data())
          ->payload_as_Reply()
          ->value_as_LayoutValidated()
          ->json()
          ->str(),
      CompleteLayout(project, "{}",
                     [&](const std::string& path) -> std::optional<LayoutFile> {
                       if (path == "definitions/wide/_card.json")
                         return LayoutFile{definition, {}};
                       return std::nullopt;
                     }));
}
}  // namespace
}  // namespace tsugumi::host

namespace tsugumi::host {
namespace {
TEST(HostLayoutSaving, RemovingAnExternalDoesNotRecreateItsLayout) {
  namespace json = ::tsugumi::json;
  const auto args =
      R"({"changes":[{"value_type":"RemoveSource","value":{"path":"externals/child/tsugumi.json","base_hash":"project"}},{"value_type":"RemoveSource","value":{"path":"externals/child/layout.json","base_hash":"layout"}}]})";
  const auto prepared = json::Object(PrepareLayoutSave(
      args,
      [](const std::string& path) -> std::optional<LayoutFile> {
        if (path == "externals/child/tsugumi.json")
          return LayoutFile{R"({"canvas":{"cards":[{"id":"a"}]}})", "project"};
        if (path == "externals/child/layout.json")
          return LayoutFile{R"({"cards":{"a":{"x":1,"y":2}}})", "layout"};
        return std::nullopt;
      },
      {"externals/child/tsugumi.json"}));
  EXPECT_EQ(prepared.at("changes"), json::Object(args).at("changes"));
}
}  // namespace
}  // namespace tsugumi::host

namespace tsugumi::host {
namespace {
TEST(HostLayoutSaving, RestrictedLayoutFileStaysDerived) {
  const std::string project = R"({"canvas":{"cards":[{"id":"a"},{"id":"b"}]}})";
  const auto saved = ::tsugumi::json::Object(PrepareLayoutSave(
      R"({"changes":[{"value_type":"PutSource","value":{"path":"definitions/a/code.py","bytes":"edit"}}]})",
      [&](const std::string& path) -> std::optional<LayoutFile> {
        if (path == "tsugumi.json") return LayoutFile{project, {}};
        if (path == "layout.json") return LayoutFile{"{}", {}};
        return std::nullopt;
      },
      {"tsugumi.json"}, [](const auto&, const auto&) { return false; }));
  EXPECT_EQ(::tsugumi::json::Array(saved.at("changes")).size(), 1u);
  EXPECT_EQ(::tsugumi::json::Object(
                ::tsugumi::json::Object(CompleteLayout(project, "{}")).at("cards"))
                .size(),
            2u);
}

// Mac and Web both send LayoutCompleteRequest. Exercise that exact handler,
// with separately serialized requests, rather than a UI stub's fixed answer.
} // namespace
} // namespace tsugumi::host

namespace tsugumi::host {
namespace {
TEST(HostLayout, NonFiniteMeasurementsNameTheirFields) {
  using Mutate = std::function<void(H::LayoutSceneT&, double)>;
  const std::vector<std::pair<std::string, Mutate>> fields{
      {"scene.grid", [](auto& s, double v) { s.grid = v; }},
      {"scene.cards[0].x", [](auto& s, double v) { s.cards[0]->x = v; }},
      {"scene.cards[0].y", [](auto& s, double v) { s.cards[0]->y = v; }},
      {"scene.cards[0].width",
       [](auto& s, double v) { s.cards[0]->width = v; }},
      {"scene.cards[0].height",
       [](auto& s, double v) { s.cards[0]->height = v; }},
      {"scene.cards[0].inputs[0].x",
       [](auto& s, double v) { s.cards[0]->inputs[0]->x = v; }},
      {"scene.cards[0].outputs[0].y",
       [](auto& s, double v) { s.cards[0]->outputs[0]->y = v; }},
      {"scene.frames[0].width",
       [](auto& s, double v) { s.frames[0]->width = v; }},
      {"scene.notes[0].height",
       [](auto& s, double v) { s.notes[0]->height = v; }},
      {"scene.inputs.x", [](auto& s, double v) { s.inputs->x = v; }},
      {"scene.outputs.y", [](auto& s, double v) { s.outputs->y = v; }},
  };
  for (const auto& [field, mutate] : fields) {
    for (const double value : {std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::infinity(),
                               -std::numeric_limits<double>::infinity()}) {
      for (const bool arrange : {false, true}) {
        SCOPED_TRACE(field);
        auto scene = std::make_unique<H::LayoutSceneT>();
        scene->flow = "topToBottom";
        auto card = std::make_unique<H::LayoutCardT>();
        card->id = "mnist";
        auto port = std::make_unique<H::LayoutPortT>();
        port->name = "data";
        card->inputs.push_back(std::make_unique<H::LayoutPortT>(*port));
        card->outputs.push_back(std::move(port));
        scene->cards.push_back(std::move(card));
        auto box = std::make_unique<H::LayoutBoxT>();
        box->id = "mnist";
        scene->frames.push_back(std::make_unique<H::LayoutBoxT>(*box));
        scene->notes.push_back(std::move(box));
        scene->inputs = std::make_unique<H::LayoutBarT>();
        scene->outputs = std::make_unique<H::LayoutBarT>();
        scene->inputs->x = scene->inputs->y = 0;
        scene->outputs->x = scene->outputs->y = 0;
        mutate(*scene, value);
        try {
          (void)arrange;
          MeasuredScene(*scene);
          FAIL() << "Accepted non-finite " << field;
        } catch (const LayoutFailure& error) {
          EXPECT_NE(std::string(error.what()).find(field + ":"),
                    std::string::npos)
              << error.what();
        }
      }
    }
  }
}

TEST(HostLayout, MissingPlacesAndUnmeasuredCardsProduceFiniteCoordinates) {
  for (const auto* flow : {"topToBottom", "leftToRight"}) {
    for (const auto* cards :
         {"[]", R"([{"id":"a"}])",
          R"([{"id":"a","width":0,"height":0},{"id":"b"}])"}) {
      SCOPED_TRACE(flow);
      SCOPED_TRACE(cards);
      std::vector<std::string> instances;
      for (const auto& raw : ::tsugumi::json::Array(cards))
        instances.push_back(::tsugumi::json::Encode(::tsugumi::json::Members{
            {"id", ::tsugumi::json::Get(::tsugumi::json::Object(raw), "id")}}));
      const auto document = ::tsugumi::json::Encode(::tsugumi::json::Members{
          {"canvas", ::tsugumi::json::Encode(::tsugumi::json::Members{
                         {"cards", ::tsugumi::json::Encode(instances)}})}});
      const auto completed = CompleteLayout(document, "{}", {});
      EXPECT_NO_THROW(ValidateLayout(completed));
      const auto input =
          SceneFixture({{"flow", ::tsugumi::json::Quote(flow)}, {"cards", cards}});
      for (const auto &proposal :
           {layout::arrange(MeasuredScene(input)),
            layout::untangle(MeasuredScene(input), {})}) {
        for (const auto &card : proposal.cards) {
          EXPECT_TRUE(std::isfinite(card.x));
          EXPECT_TRUE(std::isfinite(card.y));
        }
      }
    }
  }
}
}  // namespace
}  // namespace tsugumi::host
