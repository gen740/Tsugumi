#include "host/layout_service.hh"

#include <gtest/gtest.h>

#include "agent/catalog.hh"
#include "agent/json.hh"
#include "host/layout_saving.hh"
#include "util/fs.hh"

namespace tsugumi::host {
namespace {
namespace H = Tsugumi::Host;

TEST(HostLayout, ValidationRejectsTheOpaqueCoreVectors) {
  const auto cases = agent::json::Array(fs::read_file_or_throw(
      std::string(TSUGUMI_LAYOUT_FIXTURES) + "/layout-invalid.json"));
  ASSERT_EQ(cases.size(), 16u);
  for (const auto& raw : cases) {
    const auto value = agent::json::Object(raw);
    SCOPED_TRACE(agent::json::Text(value, "name"));
    flatbuffers::FlatBufferBuilder b;
    const auto input = H::CreateLayoutValidateRequest(
        b, b.CreateString(agent::json::Text(value, "json")));
    b.Finish(H::CreateRequest(b, H::RequestPayload::LayoutValidateRequest,
                              input.Union()));
    EXPECT_THROW(
        LayoutHandle(*flatbuffers::GetRoot<H::Request>(b.GetBufferPointer()),
                     7),
        LayoutFailure);
  }
}

TEST(HostLayout, EveryGoldenRequestReturnsACorrelatedProposal) {
  const agent::Catalog catalog;
  for (const auto& raw : agent::json::Array(fs::read_file_or_throw(
           std::string(TSUGUMI_LAYOUT_FIXTURES) + "/layout-golden.json"))) {
    const auto value = agent::json::Object(raw);
    SCOPED_TRACE(agent::json::Text(value, "name"));
    auto scene = agent::json::Object(value.at("scene"));
    std::vector<std::string> wires;
    for (const auto& wire : agent::json::Array(scene.at("wires"))) {
      auto fields = agent::json::Object(wire);
      fields["from_port"] = fields.at("fromPort");
      fields["to_port"] = fields.at("toPort");
      fields.erase("fromPort");
      fields.erase("toPort");
      wires.push_back(agent::json::Encode(fields));
    }
    scene["wires"] = agent::json::Encode(wires);
    for (const auto* side : {"inputs", "outputs"}) {
      if (!scene.contains(side)) continue;
      auto bar = agent::json::Object(scene.at(side));
      if (bar.contains("at")) {
        for (const auto& [key, coordinate] : agent::json::Object(bar.at("at")))
          bar[key] = coordinate;
        bar.erase("at");
      }
      scene[side] = agent::json::Encode(bar);
    }
    agent::json::Members args{{"scene", agent::json::Encode(scene)}};
    if (value.contains("around")) args["around"] = value.at("around");
    const auto request = catalog.HostRequest(
        agent::json::Text(value, "request") == "arrange" ? "layout_arrange"
                                                         : "layout_untangle",
        agent::json::Encode(args), 7);
    const auto reply =
        LayoutHandle(*H::GetMessage(request.data())->payload_as_Request(), 7);
    flatbuffers::Verifier verifier(
        reinterpret_cast<const uint8_t*>(reply.data()), reply.size());
    ASSERT_TRUE(H::VerifyMessageBuffer(verifier));
    const auto* message = H::GetMessage(reply.data());
    EXPECT_EQ(message->request_id(), 7u);
    const auto* proposal =
        message->payload_as_Reply()->value_as_LayoutProposal();
    ASSERT_NE(proposal, nullptr);
    const auto expected = agent::json::Object(value.at("expect"));
    auto places = [&](const auto* got, const char* key) {
      const auto positions = agent::json::Array(expected.at(key));
      ASSERT_EQ(got->size(), positions.size());
      for (size_t i = 0; i < positions.size(); ++i) {
        const auto position = agent::json::Object(positions[i]);
        EXPECT_EQ(got->Get(i)->id()->str(), agent::json::Text(position, "id"));
        EXPECT_EQ(got->Get(i)->x(), std::stod(position.at("x")));
        EXPECT_EQ(got->Get(i)->y(), std::stod(position.at("y")));
      }
    };
    places(proposal->cards(), "cards");
    places(proposal->frames(), "frames");
    places(proposal->notes(), "notes");
    const auto orders = agent::json::Array(expected.at("portOrders"));
    ASSERT_EQ(proposal->port_orders()->size(), orders.size());
    for (size_t i = 0; i < orders.size(); ++i) {
      const auto order = agent::json::Object(orders[i]);
      const auto* got = proposal->port_orders()->Get(i);
      EXPECT_EQ(got->id()->str(), agent::json::Text(order, "id"));
      for (const auto* side : {"inputs", "outputs"}) {
        const auto ports = agent::json::Array(order.at(side));
        const auto* names =
            std::string(side) == "inputs" ? got->inputs() : got->outputs();
        ASSERT_EQ(names->size(), ports.size());
        for (size_t p = 0; p < ports.size(); ++p)
          EXPECT_EQ(names->Get(p)->str(), agent::json::String(ports[p]));
      }
    }
  }
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
  const auto result = agent::json::Object(ValidateLayout(text, {"a"}));
  EXPECT_EQ(agent::json::Object(result.at("cards")).size(), 1u);
  EXPECT_TRUE(agent::json::Object(result.at("cards")).contains("ab"));
  EXPECT_TRUE(agent::json::Object(result.at("displays")).empty());
  EXPECT_TRUE(agent::json::Object(result.at("boundaries")).empty());
  const auto group =
      agent::json::Object(agent::json::Array(result.at("groups")).at(0));
  EXPECT_EQ(agent::json::Array(group.at("cards")).size(), 1u);
  EXPECT_EQ(agent::json::Array(group.at("outputs")).size(), 1u);
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

TEST(HostLayout, AgentTidyUsesHostContractAndNeverProjectLayoutRequests) {
  const agent::Catalog catalog;
  const auto* tool = catalog.Find("layout_arrange");
  ASSERT_NE(tool, nullptr);
  EXPECT_TRUE(tool->host);
  const auto request = catalog.HostRequest(
      "layout_arrange",
      R"({"scene":{"flow":"topToBottom","cards":[],"wires":[],"frames":[],"notes":[]}})",
      7);
  const auto* message = H::GetMessage(request.data());
  EXPECT_EQ(message->request_id(), 7u);
  const auto response =
      catalog.HostReply(LayoutHandle(*message->payload_as_Request(), 7));
  EXPECT_EQ(agent::json::Text(agent::json::Object(response), "reply"),
            "LayoutProposal");
  EXPECT_THROW(agent::EncodeProjectRequest("layout_arrange", "{}", 1),
               std::runtime_error);
}
}  // namespace
}  // namespace tsugumi::host

namespace tsugumi::host {
namespace {
TEST(HostLayoutSaving, RootCleanupIsPartOfTheSameSave) {
  const auto args =
      R"({"base_rev":"7","changes":[{"value_type":"SetProject","value":{"json":"{\"canvas\":{\"cards\":[]}}"}}]})";
  const auto saved = agent::json::Object(PrepareLayoutSave(
      args, [](const std::string& path) -> std::optional<LayoutFile> {
        if (path == "tsugumi.json")
          return LayoutFile{R"({"canvas":{"cards":[{"id":"a"}]}})", {}};
        if (path == "layout.json")
          return LayoutFile{R"({"cards":{"a":{"x":1,"y":2}}})", {}};
        return std::nullopt;
      }));
  EXPECT_EQ(agent::json::Text(saved, "base_rev"), "7");
  const auto changes = agent::json::Array(saved.at("changes"));
  ASSERT_EQ(changes.size(), 2u);
  const auto cleanup = agent::json::Object(changes.at(1));
  EXPECT_EQ(agent::json::Text(cleanup, "value_type"), "SetLayout");
  const auto layout = agent::json::Object(
      agent::json::Text(agent::json::Object(cleanup.at("value")), "json"));
  EXPECT_TRUE(agent::json::Object(layout.at("cards")).empty());
}

TEST(HostLayoutSaving, ExternalCleanupRetainsSourceHashGuard) {
  const auto args =
      R"({"changes":[{"value_type":"PutSource","value":{"path":"externals/child/tsugumi.json","bytes":"{\"canvas\":{\"cards\":[]}}","base_hash":"project"}}]})";
  const auto saved = agent::json::Object(PrepareLayoutSave(
      args, [](const std::string& path) -> std::optional<LayoutFile> {
        if (path == "externals/child/tsugumi.json")
          return LayoutFile{R"({"canvas":{"cards":[{"id":"a"}]}})", "project"};
        if (path == "externals/child/layout.json")
          return LayoutFile{R"({"cards":{"a":{"x":1,"y":2}}})", "layout"};
        return std::nullopt;
      }));
  const auto changes = agent::json::Array(saved.at("changes"));
  ASSERT_EQ(changes.size(), 2u);
  const auto cleanup = agent::json::Object(
      agent::json::Get(agent::json::Object(changes.at(1)), "value"));
  EXPECT_EQ(agent::json::Text(cleanup, "path"), "externals/child/layout.json");
  EXPECT_EQ(agent::json::Text(cleanup, "base_hash"), "layout");
  EXPECT_TRUE(agent::json::Object(
                  agent::json::Get(
                      agent::json::Object(agent::json::Text(cleanup, "bytes")),
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
