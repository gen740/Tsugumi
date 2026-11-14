#include "host/document_edits.hh"

#include <gtest/gtest.h>

#include "agent/json.hh"
#include "host/permission_kinds.hh"
#include "util/fs.hh"

namespace tsugumi::host {
namespace {
namespace json = agent::json;

auto Normalized(const std::string &value) -> std::string {
  const auto raw = schema::text(schema::capture(value));
  if (raw.starts_with('{')) {
    auto fields = json::Object(raw);
    for (auto &[key, item] : fields) {
      (void)key;
      item = Normalized(item);
    }
    return json::Encode(fields);
  }
  if (raw.starts_with('[')) {
    auto items = json::Array(raw);
    for (auto &item : items)
      item = Normalized(item);
    return json::Encode(items);
  }
  return raw;
}
TEST(DocumentMerge, FormerClientMergeFixtures) {
  const auto fixtures = fs::read_file(TSUGUMI_MERGE_FIXTURES);
  ASSERT_TRUE(fixtures.has_value());
  for (const auto &raw : json::Array(*fixtures)) {
    const auto fixture = json::Object(raw);
    SCOPED_TRACE(json::Text(fixture, "name"));
    const auto edit = MergeDocuments(fixture.at("base"), fixture.at("ours"),
                                     fixture.at("theirs"));
    EXPECT_EQ(Normalized(edit.merged_json), Normalized(fixture.at("expect")));
    std::vector<std::string> conflicts;
    for (const auto &path : json::Array(json::Get(fixture, "conflicts", "[]")))
      conflicts.push_back(json::String(path));
    EXPECT_EQ(edit.conflicts, conflicts);
  }
}
TEST(DocumentMerge, RunConditionsAndDeletionNeedAChoiceButLayoutDoesNot) {
  for (const auto *field :
       {"settings", "permissions", "hostFolders", "nixSources"}) {
    const auto state = [&](int value) {
      return json::Encode(json::Members{{field, std::to_string(value)}});
    };
    EXPECT_FALSE(MergeDocuments(state(1), state(2), state(3)).conflicts.empty())
        << field;
  }
  EXPECT_TRUE(MergeDocuments(R"({"layout":{"x":1}})", R"({"layout":{"x":2}})",
                             R"({"layout":{"x":3}})")
                  .conflicts.empty());
  EXPECT_FALSE(MergeDocuments(R"({"params":[{"name":"layout","data":1}]})",
                              R"({"params":[{"name":"layout","data":2}]})",
                              R"({"params":[{"name":"layout","data":3}]})")
                   .conflicts.empty());
  const auto deletion = MergeDocuments(
      R"({"definitions":{"a":{"card":{"n":1}}}})", R"({"definitions":{}})",
      R"({"definitions":{"a":{"card":{"n":2}}}})");
  EXPECT_EQ(deletion.conflicts, std::vector<std::string>({"/definitions/a"}));
  EXPECT_EQ(deletion.merged_json, R"({"definitions":{}})");
}
TEST(DocumentMerge,
     CompetingWireReplacementsOfferLocalWireWithoutCombiningThem) {
  const auto state = [](const std::string &from) {
    return R"({"connections":[{"from":")" + from + R"(","to":"input"}]})";
  };
  const auto edit = MergeDocuments(state("a"), state("b"), state("c"));
  EXPECT_EQ(edit.conflicts, std::vector<std::string>({"/connections"}));
  EXPECT_EQ(Normalized(edit.merged_json), Normalized(state("b")));
}
TEST(DocumentMerge, ObjectSpellingAndLargeIntegersArePreserved) {
  const auto edit = MergeDocuments(R"({"n":9007199254740993,"x":1})",
                                   R"({"x":1, "n":9007199254740993})",
                                   R"({"n":9007199254740993,"x":2})");
  EXPECT_TRUE(edit.conflicts.empty());
  EXPECT_EQ(edit.merged_json, R"({"n":9007199254740993,"x":2})");
}

TEST(DocumentSubgraph, ClientGestureFixturesUseActualHostReplies) {
  const auto fixtures = fs::read_file(TSUGUMI_SUBGRAPH_FIXTURES);
  ASSERT_TRUE(fixtures.has_value());
  for (const auto &raw : json::Array(*fixtures)) {
    const auto fixture = json::Object(raw);
    std::vector<std::string> cards;
    for (const auto &card : json::Array(fixture.at("cards")))
      cards.push_back(json::String(card));
    const auto edit = MakeSubgraph(json::Text(fixture, "projectJson"), cards,
                                   json::Text(fixture, "id"));
    const auto reply = json::Object(fixture.at("reply"));
    EXPECT_EQ(json::Object(edit.project_json),
              json::Object(json::Text(reply, "projectJson")));
    EXPECT_EQ(json::Object(edit.external_json),
              json::Object(json::Text(reply, "externalJson")));
    EXPECT_EQ(json::Object(edit.link_json),
              json::Object(json::Text(reply, "linkJson")));
  }
}

TEST(DocumentSubgraph, PreservesRunConditionsAndBothSidesOfBoundary) {
  const std::string root =
      R"({"name":"parent","pkgsPath":"pinned","nixSources":[{"name":"science","type":"flake","ref":"locked","overrides":{"p":{"override":{"flag":true}}}}],"settings":{"future":42},"canvas":{"cards":[{"id":"a","settings":{"sandbox":false},"params":[{"name":"value","data":7}]},{"id":"b"},{"id":"c"}],"connections":[{"from":{"card":"a","port":"x"},"to":{"card":"b","port":"in"}},{"from":{"card":"c","port":"out"},"to":{"card":"a","port":"in"}},{"from":{"card":"a","port":"x"},"to":{"card":"c","port":"in"}}]},"boundary":{"inputs":{"input":{"card":"a","port":"in"}},"outputs":{"result":{"card":"b","port":"out"}}}})";
  const auto edit = MakeSubgraph(root, {"a", "b"}, "sub");
  const auto before = json::Object(root),
             outer = json::Object(edit.project_json),
             inner = json::Object(edit.external_json);
  for (const auto *field : {"pkgsPath", "nixSources", "settings"}) {
    EXPECT_EQ(before.at(field), inner.at(field));
    EXPECT_EQ(before.at(field), outer.at(field));
  }
  const auto old_graph = json::Object(before.at("canvas"));
  const auto graph = json::Object(inner.at("canvas"));
  const auto cards = json::Array(graph.at("cards"));
  const auto originals = json::Array(old_graph.at("cards"));
  EXPECT_EQ(cards, (std::vector<std::string>{originals[0], originals[1]}));
  EXPECT_EQ(json::Array(graph.at("connections")).size(), 1);
  const auto boundary = json::Object(inner.at("boundary"));
  EXPECT_EQ(json::Object(boundary.at("inputs")).size(), 1);
  EXPECT_EQ(json::Object(boundary.at("outputs")).size(), 2);
  const auto outer_boundary = json::Object(outer.at("boundary"));
  for (const auto *side : {"inputs", "outputs"})
    for (const auto &[name, endpoint] : json::Object(outer_boundary.at(side))) {
      (void)name;
      EXPECT_EQ(json::Text(json::Object(endpoint), "card"), "sub");
      const auto port = json::Text(json::Object(endpoint), "port");
      EXPECT_TRUE(json::Object(boundary.at(side)).contains(port));
    }
  EXPECT_EQ(
      json::Array(json::Object(outer.at("canvas")).at("connections")).size(),
      2);
}

TEST(DocumentSubgraph, ReusesEndpointsAndDisambiguatesPortNames) {
  const auto edit = MakeSubgraph(
      R"({"canvas":{"cards":[{"id":"a_b"},{"id":"a"},{"id":"outside"}],"connections":[{"from":{"card":"a_b","port":"c"},"to":{"card":"outside","port":"one"}},{"from":{"card":"a","port":"b_c"},"to":{"card":"outside","port":"two"}},{"from":{"card":"a_b","port":"c"},"to":{"card":"outside","port":"three"}}]}})",
      {"a_b", "a"}, "sub");
  const auto boundary =
      json::Object(json::Object(edit.external_json).at("boundary"));
  const auto ports = json::Object(boundary.at("outputs"));
  EXPECT_EQ(ports.size(), 2);
  EXPECT_TRUE(ports.contains("a_b_c"));
  EXPECT_TRUE(ports.contains("a_b_c2"));
  const auto wires =
      json::Array(json::Object(json::Object(edit.project_json).at("canvas"))
                      .at("connections"));
  EXPECT_EQ(json::Get(json::Object(wires[0]), "from"),
            json::Get(json::Object(wires[2]), "from"));
}

TEST(DocumentSubgraph, RejectsInvalidSelectionAndIdWithoutPartialEdits) {
  const std::string root =
      R"({"canvas":{"cards":[{"id":"a"}],"connections":[]}})";
  EXPECT_THROW(MakeSubgraph(root, {}, "sub"), std::runtime_error);
  EXPECT_THROW(MakeSubgraph(root, {"missing"}, "sub"), std::runtime_error);
  EXPECT_THROW(MakeSubgraph(root, {"a"}, "a"), std::runtime_error);
  EXPECT_THROW(MakeSubgraph(root, {"a"}, "../sub"), std::runtime_error);
  EXPECT_NO_THROW(MakeSubgraph(root, {"a"}, "sub"));
}
} // namespace
} // namespace tsugumi::host

TEST(HostPermissionKinds, SuppliesIdenticalIdsAndLocalizedDescriptions) {
  const auto ja = tsugumi::host::PermissionKinds("ja");
  const auto en = tsugumi::host::PermissionKinds("en");
  ASSERT_EQ(ja.kinds.size(), en.kinds.size());
  bool execution = false;
  for (size_t i = 0; i < ja.kinds.size(); ++i) {
    EXPECT_EQ(ja.kinds[i]->id, en.kinds[i]->id);
    EXPECT_EQ(ja.kinds[i]->icon, en.kinds[i]->icon);
    EXPECT_FALSE(ja.kinds[i]->label.empty());
    EXPECT_FALSE(en.kinds[i]->label.empty());
    EXPECT_NE(ja.kinds[i]->label, en.kinds[i]->label);
    if (en.kinds[i]->id == "execution_service") {
      execution = true;
      EXPECT_NE(en.kinds[i]->label.find("data"), std::string::npos);
    }
  }
  EXPECT_TRUE(execution);
}
