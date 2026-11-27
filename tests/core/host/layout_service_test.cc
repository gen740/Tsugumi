#include "host/layout_file.hh"
#include "host/layout_service.hh"

#include <gtest/gtest.h>

#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <set>

#include "agent/catalog.hh"
#include "host/layout_saving.hh"
#include "host/save.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/util/json_object.hh"

namespace tsugumi::host {
namespace {
namespace H = Tsugumi::Host;

TEST(HostLayout, ValidationRejectsTheOpaqueCoreVectors) {
  const auto cases = ::tsugumi::json::Array(fs::read_file_or_throw(
      std::string(TSUGUMI_LAYOUT_FIXTURES) + "/layout-invalid.json"));
  ASSERT_FALSE(cases.empty());
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

auto SceneFixture(const ::tsugumi::json::Members& value) -> H::LayoutSceneT {
  namespace json = ::tsugumi::json;
  H::LayoutSceneT scene;
  scene.flow = json::Text(value, "flow");
  scene.grid = std::stod(json::Get(value, "grid", "0"));
  scene.snap = json::Bool(json::Get(value, "snap", "false"));
  auto coordinate = [&](const auto& fields, const auto& key) {
    return std::stod(json::Get(fields, key, "0"));
  };
  auto ports = [&](const std::string& raw, auto& result) {
    for (const auto& item : json::Array(raw)) {
      const auto fields = json::Object(item);
      auto port = std::make_unique<H::LayoutPortT>();
      port->name = json::Text(fields, "name");
      port->x = coordinate(fields, "x");
      port->y = coordinate(fields, "y");
      result.push_back(std::move(port));
    }
  };
  for (const auto& raw : json::Array(json::Get(value, "cards", "[]"))) {
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
  for (const auto& raw : json::Array(json::Get(value, "wires", "[]"))) {
    const auto fields = json::Object(raw);
    auto wire = std::make_unique<H::LayoutWireT>();
    wire->from = json::Text(fields, "from");
    wire->from_port = json::Text(fields, "fromPort");
    wire->to = json::Text(fields, "to");
    wire->to_port = json::Text(fields, "toPort");
    scene.wires.push_back(std::move(wire));
  }
  auto boxes = [&](const auto& field, auto& result) {
    for (const auto& raw : json::Array(json::Get(value, field, "[]"))) {
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
  boxes("notes", scene.notes);
  auto bar = [&](const auto& side, auto& result) {
    if (!value.contains(side)) return;
    result = std::make_unique<H::LayoutBarT>();
    const auto fields = json::Object(value.at(side));
    if (fields.contains("at")) {
      const auto at = json::Object(fields.at("at"));
      result->x = coordinate(at, "x");
      result->y = coordinate(at, "y");
    }
    for (const auto& raw : json::Array(json::Get(fields, "ends", "[]"))) {
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
TEST(HostLayout, ScenesPreserveLayoutConstraints) {
  namespace json = ::tsugumi::json;
  for (const auto& raw : json::Array(fs::read_file_or_throw(
           std::string(TSUGUMI_LAYOUT_FIXTURES) + "/layout-scenes.json"))) {
    const auto value = json::Object(raw);
    SCOPED_TRACE(json::Text(value, "name"));
    const auto scene =
        MeasuredScene(SceneFixture(json::Object(value.at("scene"))));
    std::vector<std::string> around;
    for (const auto& id : json::Array(json::Get(value, "around", "[]")))
      around.push_back(json::String(id));
    const bool arrange = json::Text(value, "request") == "arrange";
    const auto proposal =
        arrange ? layout::arrange(scene) : layout::untangle(scene, around);
    std::vector<layout::Rect> occupied;
    std::map<std::string, layout::Point> positions;
    const auto places = [&](const auto& original, const auto& proposed) {
      std::set<std::string> changed;
      for (const auto& p : proposed) {
        EXPECT_TRUE(std::isfinite(p.x) && std::isfinite(p.y));
        EXPECT_TRUE(changed.insert(p.id).second);
        EXPECT_TRUE(std::any_of(original.begin(), original.end(),
                                [&](const auto& o) { return o.id == p.id; }));
      }
      if (arrange)
        EXPECT_EQ(changed.size(), original.size());
      else
        EXPECT_TRUE(
            proposed.empty());  // Untangling changes ports, never positions.
      for (const auto& o : original) {
        const auto p =
            std::find_if(proposed.begin(), proposed.end(),
                         [&](const auto& p) { return p.id == o.id; });
        const auto x = p == proposed.end() ? o.x : p->x,
                   y = p == proposed.end() ? o.y : p->y;
        occupied.push_back({x, y, o.width, o.height});
      }
    };
    places(scene.cards, proposal.cards);
    places(scene.notes, proposal.notes);
    if (arrange) {
      for (size_t i = 0; i < occupied.size(); ++i)
        for (size_t j = i + 1; j < occupied.size(); ++j) {
          const auto& a = occupied[i];
          const auto& b = occupied[j];
          EXPECT_FALSE(a.x < b.x + b.width && b.x < a.x + a.width &&
                       a.y < b.y + b.height && b.y < a.y + a.height);
        }
    }
    for (const auto& c : scene.cards) positions[c.id] = {c.x, c.y};
    for (const auto& c : proposal.cards) positions[c.id] = {c.x, c.y};
    for (const auto& raw : json::Array(json::Get(value, "forward", "[]"))) {
      const auto pair = json::Array(raw);
      const auto a = positions.at(json::String(pair[0])),
                 b = positions.at(json::String(pair[1]));
      EXPECT_LT(scene.top_to_bottom ? a.y : a.x,
                scene.top_to_bottom ? b.y : b.x);
    }
    std::set<std::string> affected(around.begin(), around.end());
    for (const auto& wire : scene.wires)
      if (std::ranges::find(around, wire.from) != around.end() ||
          std::ranges::find(around, wire.to) != around.end()) {
        affected.insert(wire.from);
        affected.insert(wire.to);
      }
    for (const auto& order : proposal.port_orders) {
      const auto card =
          std::find_if(scene.cards.begin(), scene.cards.end(),
                       [&](const auto& c) { return c.id == order.id; });
      ASSERT_NE(card, scene.cards.end());
      if (!arrange && !around.empty()) EXPECT_TRUE(affected.contains(order.id));
      const auto permutation = [&](const auto& ports, const auto& names) {
        std::multiset<std::string> expected;
        for (const auto& port : ports) expected.insert(port.name);
        EXPECT_EQ(expected,
                  (std::multiset<std::string>(names.begin(), names.end())));
      };
      permutation(card->inputs, order.inputs);
      permutation(card->outputs, order.outputs);
    }
  }
}

TEST(HostLayout, OmittedVersionCanvasVectorsAreEmpty) {
  flatbuffers::FlatBufferBuilder b;
  b.Finish(H::CreateVersionCanvasRequest(b, b.CreateVector(std::vector<uint8_t>{})));
  const auto result = VersionCanvas(
      *flatbuffers::GetRoot<H::VersionCanvasRequest>(b.GetBufferPointer()));
  EXPECT_TRUE(result.cards.empty());
  EXPECT_TRUE(result.wires.empty());
  EXPECT_EQ(result.flow, "leftToRight");
}

TEST(HostLayout, SchemaRejectsIncompleteAndUnknownPresentationFields) {
  EXPECT_EQ(ValidateLayout(" { } "), " { } ");
  for (const auto* json :
       {R"({"cards":{"a":{"x":1}}})", R"({"notes":[{"id":"n","x":1}]})",
        R"({"displays":{"a":{"x":1,"y":2}}})", R"({"groups":[{"typo":true}]})",
        "[]"})
    EXPECT_THROW(ValidateLayout(json), LayoutFailure) << json;
}

TEST(HostLayout, SavingClientRemovesOnlyDeletedCardReferences) {
  const auto text =
      R"({"cards":{"a":{"x":1,"y":2},"a/child":{"x":3,"y":4},"ab":{"x":5,"y":6}},"boundaries":{"a":{"inputs":{"x":1,"y":2}}},"groups":[{"id":"g","cards":["a","ab"],"outputs":[{"card":"a","port":"x"},{"card":"ab","port":"y"}]}]})";
  const auto result = ::tsugumi::json::Object(ValidateLayout(text, {"a"}));
  EXPECT_EQ(::tsugumi::json::Object(result.at("cards")).size(), 1u);
  EXPECT_TRUE(::tsugumi::json::Object(result.at("cards")).contains("ab"));
  EXPECT_TRUE(::tsugumi::json::Object(result.at("boundaries")).empty());
  const auto group = ::tsugumi::json::Object(
      ::tsugumi::json::Array(result.at("groups")).at(0));
  EXPECT_EQ(::tsugumi::json::Array(group.at("cards")).size(), 1u);
  EXPECT_EQ(::tsugumi::json::Array(group.at("outputs")).size(), 1u);
}

auto canvas(const std::string& layout) -> H::VersionCanvasT {
  flatbuffers::FlatBufferBuilder b;
  auto a = H::CreateVersionCanvasCard(b, b.CreateString("a"), 0, 0,
                                      b.CreateString("input"));
  auto c = H::CreateVersionCanvasCard(b, b.CreateString("b"), 0, 0,
                                      b.CreateString("merge"), 3);
  auto w =
      H::CreateVersionCanvasWire(b, b.CreateString("a"), b.CreateString("b"));
  const auto bytes = layout == "corrupt" ? layout : SplitLayoutView(layout).layout;
  auto req = H::CreateVersionCanvasRequest(b, b.CreateVector(
      reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()),
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

TEST(HostLayout, VersionCanvasCompletesOnlyMissingPositions) {
  const auto result = canvas(
      R"({"flow":"topToBottom","cards":{"a":{"x":407,"y":609,"collapsed":true},"b":{"collapsed":true}}})");
  EXPECT_EQ(result.flow, "topToBottom");
  const auto& a = *result.cards.at(0);
  const auto& b = *result.cards.at(1);
  EXPECT_DOUBLE_EQ(a.x, 407);
  EXPECT_DOUBLE_EQ(a.y, 609);
  EXPECT_TRUE(a.collapsed);
  EXPECT_TRUE(b.collapsed);
  EXPECT_TRUE(std::abs(a.x - b.x) >= 320 || std::abs(a.y - b.y) >= 120);
}

TEST(HostLayout, VersionCanvasFallsBackForMissingOrInvalidLayout) {
  for (const auto* layout : {"{}", "corrupt"}) {
    const auto result = canvas(layout);
    EXPECT_EQ(result.flow, "leftToRight");
    const auto& a = *result.cards.at(0);
    const auto& b = *result.cards.at(1);
    EXPECT_TRUE(std::abs(a.x - b.x) >= 320 || std::abs(a.y - b.y) >= 120);
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
        if (path == "layout.bin")
          return LayoutFile{R"({"cards":{"a":{"x":1,"y":2}}})", {}};
        return std::nullopt;
      }));
  EXPECT_EQ(::tsugumi::json::Text(saved, "base_rev"), "7");
  const auto changes = ::tsugumi::json::Array(saved.at("changes"));
  ASSERT_EQ(changes.size(), 2u);
  const auto cleanup = ::tsugumi::json::Object(changes.at(1));
  EXPECT_EQ(::tsugumi::json::Text(cleanup, "value_type"), "SetLayout");
  const auto value = ::tsugumi::json::Object(cleanup.at("value"));
  const auto layout = ::tsugumi::json::Object(
      LayoutView(DecodeSourceBytes(value.at("layout"))));
  EXPECT_TRUE(::tsugumi::json::Object(
      ::tsugumi::json::Get(layout, "cards", "{}")).empty());
}

TEST(HostLayoutSaving, ExternalCleanupRetainsSourceHashGuard) {
  const auto args =
      R"({"changes":[{"value_type":"PutSource","value":{"path":"externals/child/tsugumi.json","bytes":"{\"canvas\":{\"cards\":[]}}","base_hash":"project"}}]})";
  const auto saved = ::tsugumi::json::Object(PrepareLayoutSave(
      args, [](const std::string& path) -> std::optional<LayoutFile> {
        if (path == "externals/child/tsugumi.json")
          return LayoutFile{R"({"canvas":{"cards":[{"id":"a"}]}})", "project"};
        if (path == "externals/child/layout.bin")
          return LayoutFile{R"({"cards":{"a":{"x":1,"y":2}}})", "layout"};
        return std::nullopt;
      }));
  const auto changes = ::tsugumi::json::Array(saved.at("changes"));
  ASSERT_EQ(changes.size(), 2u);
  const auto cleanup = ::tsugumi::json::Object(
      ::tsugumi::json::Get(::tsugumi::json::Object(changes.at(1)), "value"));
  EXPECT_EQ(::tsugumi::json::Text(cleanup, "path"),
            "externals/child/layout.bin");
  EXPECT_EQ(::tsugumi::json::Text(cleanup, "base_hash"), "layout");
  const auto layout = ::tsugumi::json::Object(
      LayoutView(DecodeSourceBytes(cleanup.at("bytes"))));
  EXPECT_TRUE(::tsugumi::json::Object(
      ::tsugumi::json::Get(layout, "cards", "{}")).empty());
}

TEST(HostLayoutSaving, PatchesAreValidatedAndSourceLookalikesAreUnchanged) {
  const auto read = [](const std::string&) -> std::optional<LayoutFile> {
    return LayoutFile{"{}", "hash"};
  };
  const auto invalid =
      R"({"changes":[{"value_type":"PatchSource","value":{"path":"externals/child/layout.bin","base_hash":"hash","patches":[{"offset":"1","remove":"0","text":"\"cards\":1"}]}}]})";
  EXPECT_THROW(PrepareLayoutSave(invalid, read), LayoutFailure);
  const auto source =
      R"({"changes":[{"value_type":"PutSource","value":{"path":"definitions/a/src/layout.bin","bytes":"not JSON"}}]})";
  EXPECT_NO_THROW(PrepareLayoutSave(source, read));
  EXPECT_TRUE(
      IsExternalDocument("externals/a/externals/b/layout.bin", "layout.bin"));
  EXPECT_FALSE(IsExternalDocument("externals/a/definitions/b/src/layout.bin",
                                  "layout.bin"));
  EXPECT_FALSE(IsExternalDocument("layout.bin", "layout.bin"));
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
       R"([{"from":{"card":"fixed"},"to":{"card":"new"}}])",
       R"([{"from":{"card":"fixed"},"to":{"card":"new"}},{"from":{"card":"other"},"to":{"card":"new"}}])"}) {
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
TEST(HostCardPlacement, APersonsDropIsSavedWithoutAddingDerivedPositions) {
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
                    {"path", json::Quote("externals/inner/layout.bin")},
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
      json::Object(json::Object(LayoutView(DecodeSourceBytes(value.at("bytes"))))
                       .at("cards"));
  const auto dropped = json::Object(positions.at("dropped"));
  EXPECT_DOUBLE_EQ(std::stod(dropped.at("x")), 17);
  EXPECT_DOUBLE_EQ(std::stod(dropped.at("y")), 29);
  EXPECT_FALSE(positions.contains("new"));
}
}  // namespace
}  // namespace tsugumi::host

namespace tsugumi::host {
namespace {
TEST(HostCardPlacement, PlacesNearItsNeighbourAndAvoidsCardsAndNotes) {
  namespace json = ::tsugumi::json;
  const auto project =
      R"({"canvas":{"cards":[{"id":"far"},{"id":"near"},{"id":"new"}],"connections":[{"from":{"card":"near","port":"out"},"to":{"card":"new","port":"in"}}]}})";
  const auto layout =
      R"({"cards":{"far":{"x":10000,"y":0},"near":{"x":0,"y":0}},"notes":[{"id":"n","text":"Keep me","x":0,"y":300,"width":300,"height":200}]})";
  const auto completed = json::Object(CompleteLayout(project, layout));
  const auto positions = json::Object(completed.at("cards"));
  const auto p = json::Object(positions.at("new"));
  const auto x = std::stod(p.at("x")), y = std::stod(p.at("y"));
  EXPECT_LT(std::abs(x) + std::abs(y), 2000);
  auto disjoint = [&](double bx, double by, double w, double h) {
    return x + 320 <= bx || bx + w <= x || y + 270 <= by || by + h <= y;
  };
  EXPECT_TRUE(disjoint(0, 0, 320, 270));
  EXPECT_TRUE(disjoint(0, 300, 300, 200));
  EXPECT_FALSE(completed.contains("displays"));
  EXPECT_EQ(completed.at("notes"), json::Object(layout).at("notes"));
}
}  // namespace
}  // namespace tsugumi::host

namespace tsugumi::host {
namespace {
TEST(HostLayoutSaving, SourceSaveLeavesEveryLayoutUntouched) {
  namespace json = ::tsugumi::json;
  const auto args =
      R"({"base_rev":"7","changes":[{"value_type":"PutSource","value":{"path":"externals/one/externals/two/definitions/a/code.py","bytes":"source"}}]})";
  const auto saved = PrepareLayoutSave(
      args, [](const std::string& path) -> std::optional<LayoutFile> {
        ADD_FAILURE() << "Read an unrelated file: " << path;
        return std::nullopt;
      });
  EXPECT_EQ(json::Object(saved), json::Object(args));
}

TEST(HostCardPlacement, DisplayRpcCompletesMissingPositions) {
  namespace json = ::tsugumi::json;
  const std::string project =
      R"({"canvas":{"cards":[{"id":"wide"},{"id":"new"}],"connections":[]}})";
  const std::string definition = R"({"inputs":[{},{},{},{},{},{},{},{}]})";
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
      R"({"changes":[{"value_type":"RemoveSource","value":{"path":"externals/child/tsugumi.json","base_hash":"project"}},{"value_type":"RemoveSource","value":{"path":"externals/child/layout.bin","base_hash":"layout"}}]})";
  const auto prepared = json::Object(PrepareLayoutSave(
      args, [](const std::string& path) -> std::optional<LayoutFile> {
        if (path == "externals/child/tsugumi.json")
          return LayoutFile{R"({"canvas":{"cards":[{"id":"a"}]}})", "project"};
        if (path == "externals/child/layout.bin")
          return LayoutFile{R"({"cards":{"a":{"x":1,"y":2}}})", "layout"};
        return std::nullopt;
      }));
  EXPECT_EQ(prepared.at("changes"), json::Object(args).at("changes"));
}
}  // namespace
}  // namespace tsugumi::host

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
      {"scene.notes[0].height",
       [](auto& s, double v) { s.notes[0]->height = v; }},
      {"scene.inputs.x", [](auto& s, double v) { s.inputs->x = v; }},
      {"scene.outputs.y", [](auto& s, double v) { s.outputs->y = v; }},
  };
  for (const auto& [field, mutate] : fields) {
    for (const double value : {std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::infinity(),
                               -std::numeric_limits<double>::infinity()}) {
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
      scene->notes.push_back(std::move(box));
      scene->inputs = std::make_unique<H::LayoutBarT>();
      scene->outputs = std::make_unique<H::LayoutBarT>();
      scene->inputs->x = scene->inputs->y = 0;
      scene->outputs->x = scene->outputs->y = 0;
      mutate(*scene, value);
      EXPECT_THROW(MeasuredScene(*scene), LayoutFailure);
    }
  }
}

TEST(HostLayout, MovingACardStartsAtItsDerivedPosition) {
  namespace json = ::tsugumi::json;
  const std::string project =
      R"({"canvas":{"cards":[{"id":"fixed"},{"id":"new"}],"connections":[{"from":{"card":"fixed","port":"out"},"to":{"card":"new","port":"in"}}]}})";
  const std::string saved = R"({"cards":{"fixed":{"x":500,"y":700}}})";
  const auto position = [](const std::string& layout) {
    return json::Object(
        json::Object(json::Object(layout).at("cards")).at("new"));
  };
  const auto displayed = position(CompleteLayout(project, saved));
  H::ProjectMoveT move;
  move.cards = {"new"};
  move.dx = 100;
  move.dy = -20;
  const auto moved = position(MoveProjectLayout(project, saved, {}, move));
  EXPECT_DOUBLE_EQ(std::stod(moved.at("x")),
                   std::stod(displayed.at("x")) + move.dx);
  EXPECT_DOUBLE_EQ(std::stod(moved.at("y")),
                   std::stod(displayed.at("y")) + move.dy);
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
      const auto input = SceneFixture(
          {{"flow", ::tsugumi::json::Quote(flow)}, {"cards", cards}});
      for (const auto& proposal :
           {layout::arrange(MeasuredScene(input)),
            layout::untangle(MeasuredScene(input), {})}) {
        for (const auto& card : proposal.cards) {
          EXPECT_TRUE(std::isfinite(card.x));
          EXPECT_TRUE(std::isfinite(card.y));
        }
      }
    }
  }
}
}  // namespace
}  // namespace tsugumi::host

namespace tsugumi::host {
namespace {
TEST(HostCardPlacement, NewWiredCardsAreDrawnByDataFlowBesideTheOthers) {
  namespace json = ::tsugumi::json;
  // An Agent adds a chain fed by a placed Card and a separate pair, with no
  // positions: each chain reads down the flow, nothing overlaps, and the
  // placed Cards stay where they are.
  const std::string project = R"({"canvas":{"cards":[
    {"id":"source"},{"id":"other"},
    {"id":"a"},{"id":"b"},{"id":"c"},{"id":"d"},{"id":"e"},{"id":"f"}],
    "connections":[
      {"from":{"card":"source","port":"out"},"to":{"card":"a","port":"in"}},
      {"from":{"card":"a","port":"out"},"to":{"card":"b","port":"in"}},
      {"from":{"card":"b","port":"out"},"to":{"card":"c","port":"in"}},
      {"from":{"card":"a","port":"out"},"to":{"card":"d","port":"in"}},
      {"from":{"card":"e","port":"out"},"to":{"card":"f","port":"in"}}]}})";
  const std::string layout =
      R"({"cards":{"source":{"x":0,"y":0},"other":{"x":600,"y":0}}})";
  const auto positions =
      json::Object(json::Object(CompleteLayout(project, layout)).at("cards"));
  auto at = [&](const std::string& id) {
    const auto p = json::Object(positions.at(id));
    return std::pair{std::stod(p.at("x")), std::stod(p.at("y"))};
  };
  EXPECT_EQ(at("source"), (std::pair{0.0, 0.0}));
  EXPECT_EQ(at("other"), (std::pair{600.0, 0.0}));
  // The host reserves at least 320 x 270 for a Card it has not measured.
  EXPECT_GE(at("a").second, 270);
  EXPECT_GT(at("b").second, at("a").second);
  EXPECT_GT(at("c").second, at("b").second);
  EXPECT_GT(at("d").second, at("a").second);
  EXPECT_EQ(at("b").second, at("d").second);
  EXPECT_GT(at("f").second, at("e").second);
  for (const auto& [id, a] : positions)
    for (const auto& [other, b] : positions) {
      if (id >= other) continue;
      const auto [ax, ay] = at(id);
      const auto [bx, by] = at(other);
      EXPECT_TRUE(std::abs(ax - bx) >= 320 || std::abs(ay - by) >= 270)
          << id << " overlaps " << other;
    }
}
}  // namespace
}  // namespace tsugumi::host
