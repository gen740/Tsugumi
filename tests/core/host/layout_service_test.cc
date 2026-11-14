#include "host/layout_service.hh"

#include <gtest/gtest.h>

#include "agent/catalog.hh"
#include "agent/json.hh"
#include "host/layout_saving.hh"

namespace tsugumi::host {
namespace {
namespace H = Tsugumi::Host;

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
