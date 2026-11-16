#include "host/document_edits.hh"
#include "host/templates.hh"
#include <algorithm>

#include <gtest/gtest.h>
#include <set>

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

auto ConnectionWires(const std::string &state) -> std::set<std::string> {
  const auto project = json::Object(json::Object(state).at("project"));
  std::set<std::string> result;
  for (const auto &wire :
       json::Array(json::Object(project.at("canvas")).at("connections"))) {
    const auto w = json::Object(wire), from = json::Object(w.at("from")),
               to = json::Object(w.at("to"));
    result.insert(json::Text(from, "card") + "." + json::Text(from, "port") +
                  ">" + json::Text(to, "card") + "." + json::Text(to, "port"));
  }
  return result;
}
TEST(DocumentConnect, SecondWireInsertsForkAndLaterWiresExtendIt) {
  const std::string state =
      R"({"project":{"pkgsPath":"pinned","canvas":{"cards":[{"id":"start"},{"id":"a"},{"id":"b"},{"id":"c"}],"connections":[]}},"layout":{"cards":{"start":{"x":0,"y":20},"a":{"x":600,"y":100}}},"definitions":{"start":{"card":{"template":"trigger"}},"a":{"card":{}},"b":{"card":{}},"c":{"card":{}}}})";
  const auto first = ConnectDocuments(state, "start", "out", "a", "in", "");
  EXPECT_FALSE(first.needs_fork);
  const auto plan =
      ConnectDocuments(first.state_json, "start", "out", "b", "in", "");
  EXPECT_TRUE(plan.needs_fork);
  const auto second =
      ConnectDocuments(first.state_json, "start", "out", "b", "in", "fork");
  EXPECT_EQ(second.inserted_id, "fork");
  EXPECT_EQ(ConnectionWires(second.state_json),
            (std::set<std::string>{"start.out>fork.in", "fork.a>a.in",
                                   "fork.b>b.in"}));
  // Every newly inserted Card follows the host's missing-place rule on save.
  EXPECT_EQ(json::Object(second.state_json).at("layout"),
            json::Object(state).at("layout"));
  const auto third =
      ConnectDocuments(second.state_json, "start", "out", "c", "in", "");
  EXPECT_FALSE(third.needs_fork);
  EXPECT_TRUE(ConnectionWires(third.state_json).contains("fork.c>c.in"));
  const auto branch =
      ConnectDocuments(third.state_json, "fork", "a", "c", "other", "");
  EXPECT_TRUE(ConnectionWires(branch.state_json).contains("fork.c_2>c.other"));
  EXPECT_EQ(json::Object(json::Object(branch.state_json).at("project"))
                .at("pkgsPath"),
            json::Quote("pinned"));
}
TEST(DocumentConnect, InputReplacementKeepsDraftDefinitionsUnchanged) {
  const std::string state =
      R"({"project":{"canvas":{"cards":[{"id":"old"},{"id":"new"},{"id":"dest"}],"connections":[{"from":{"card":"old","port":""},"to":{"card":"dest","port":"in"}}]}},"definitions":{"old":{"card":{"template":"draft"},"meta":{"content":"planned"}},"new":{"card":{"template":"trigger"}},"dest":{"card":{"language":"python"}}}})";
  const auto replaced = ConnectDocuments(state, "new", "out", "dest", "in", "");
  EXPECT_EQ(ConnectionWires(replaced.state_json),
            (std::set<std::string>{"new.out>dest.in"}));
  EXPECT_EQ(Normalized(json::Object(replaced.state_json).at("definitions")),
            Normalized(json::Object(state).at("definitions")));
}
TEST(DocumentConnect,
     DraftGesturesKeepMultipleIncomingAndOutgoingWiresPortless) {
  const std::string state =
      R"({"project":{"canvas":{"cards":[{"id":"a"},{"id":"b"},{"id":"c"}],"connections":[]}},"definitions":{"a":{"card":{"template":"draft"}},"b":{"card":{"template":"draft"}},"c":{"card":{"template":"draft"}}}})";
  const auto first = ConnectDocuments(state, "a", "+", "b", "+", "");
  EXPECT_EQ(ConnectionWires(first.state_json),
            (std::set<std::string>{"a.>b."}));
  const auto second =
      ConnectDocuments(first.state_json, "a", "+", "c", "+", "");
  EXPECT_FALSE(second.needs_fork);
  const auto third =
      ConnectDocuments(second.state_json, "c", "+", "b", "+", "");
  EXPECT_FALSE(third.needs_fork);
  EXPECT_EQ(ConnectionWires(third.state_json),
            (std::set<std::string>{"a.>b.", "a.>c.", "c.>b."}));
  EXPECT_EQ(Normalized(json::Object(third.state_json).at("definitions")),
            Normalized(json::Object(state).at("definitions")));
  EXPECT_THROW(ConnectDocuments(state, "missing", "out", "b", "+", ""),
               std::runtime_error);
}

TEST(DocumentTemplate,
     BuiltinsAndCodeTemplatesHaveTheirSharedInitialDefinition) {
  const auto draft =
      json::Object(WriteTemplate("draft", "new-card").definition_json);
  EXPECT_EQ(json::Text(json::Object(draft.at("card")), "template"), "draft");
  EXPECT_EQ(json::Text(json::Object(draft.at("meta")), "content"), "");
  for (const auto *name :
       {"trigger", "fork", "branch", "collect", "merge", "input", "switch"}) {
    const auto reply = WriteTemplate(name, "new-card.2");
    const auto card =
        json::Object(json::Object(reply.definition_json).at("card"));
    EXPECT_EQ(json::Text(card, "template"), name);
    if (std::string_view(name) == "merge")
      EXPECT_EQ(json::Array(card.at("inputs")).size(), 2);
    if (std::string_view(name) == "input") {
      ASSERT_EQ(reply.files.size(), 1);
      EXPECT_NE(reply.files[0]->text.find("namespace new_card_2;"),
                std::string::npos);
      EXPECT_EQ(reply.files[0]->text.find("@NAMESPACE@"), std::string::npos);
    }
  }
  for (const auto *name : {"python", "go", "rust", "ruby", "cpp", "csharp"}) {
    const auto reply = WriteTemplate(name, "new-card.2");
    const auto card =
        json::Object(json::Object(reply.definition_json).at("card"));
    EXPECT_EQ(json::Text(card, "language"), name);
    EXPECT_EQ(json::Array(card.at("inputs")).size(), 1);
    std::set<std::string> paths;
    for (const auto &file : reply.files) {
      paths.insert(file->path);
      EXPECT_FALSE(file->path.starts_with('/'));
      EXPECT_EQ(file->text.find("@NAMESPACE@"), std::string::npos);
      if (file->path == "src/main.py")
        EXPECT_NE(file->text.find("outputs.out.new_card_2.Text"),
                  std::string::npos);
    }
    EXPECT_TRUE(paths.contains("_outputs.fbs"));
    EXPECT_TRUE(paths.contains("default.nix"));
    EXPECT_TRUE(std::ranges::any_of(
        paths, [](const auto &path) { return path.starts_with("src/"); }));
    if (std::string_view(name) == "go") {
      EXPECT_TRUE(paths.contains("src/go.mod"));
      EXPECT_TRUE(paths.contains("src/go.sum"));
    }
    if (std::string_view(name) == "rust")
      EXPECT_TRUE(paths.contains("src/Cargo.toml"));
  }
  EXPECT_THROW(WriteTemplate("missing", "new-card"), std::runtime_error);
}
TEST(DocumentTemplate,
     NotebookReportsAndPlotsKeepTheirSourceAndArtifactContracts) {
  for (const auto *name : {"marimo", "report_rmarkdown", "report_quarto",
                           "visualization_matplotlib", "visualization_plotly",
                           "visualization_altair"}) {
    const auto reply = WriteTemplate(name, "new-card");
    const auto card =
        json::Object(json::Object(reply.definition_json).at("card"));
    EXPECT_EQ(card.at("retainArtifact"), "true");
    const bool plot = std::string_view(name).starts_with("visualization_");
    EXPECT_EQ(json::Text(card, "artifactOutput"), plot ? "figure" : "report");
    std::set<std::string> files;
    for (const auto &file : reply.files)
      files.insert(file->path);
    EXPECT_TRUE(files.contains("default.nix"));
    EXPECT_TRUE(files.contains("_outputs.fbs"));
    if (std::string_view(name) == "marimo") {
      EXPECT_EQ(card.at("reuseWorker"), "false");
      EXPECT_TRUE(files.contains("src/notebook.py"));
    }
    if (plot) {
      EXPECT_TRUE(files.contains("src/main.py"));
      const auto params = json::Array(card.at("params"));
      EXPECT_TRUE(std::ranges::any_of(params, [](const auto &raw) {
        const auto param = json::Object(raw);
        return json::Text(param, "name") == "kind" &&
               json::Text(param, "data") == "line";
      }));
    }
  }
}

TEST(DocumentCopy, DefinitionKeepsSettingsAndGivesCommentsTheirOwnIdentity) {
  const std::string state =
      R"({"definitions":{"a":{"card":{"language":"python","params":[{"name":"n","data":9007199254740993}],"settings":{"sandbox":false}},"meta":{"name":"original","description":"Plots","comments":[{"id":"old","text":"one","author":"user","resolved":true,"cards":["a"]}]}}}})";
  const auto edit =
      CopyDocuments(state,
                    {"definitions/a/src/asset.bin", "definitions/a/_card.json",
                     "definitions/a/_meta.json", "definitions/ab/no"},
                    "definitions/a/", "definitions/copy/", "new name");
  const auto defs =
      json::Object(json::Object(edit.edits_json).at("definitions"));
  const auto draft = json::Object(defs.at("definitions/copy/"));
  EXPECT_EQ(Normalized(draft.at("card")),
            Normalized(
                json::Object(
                    json::Object(json::Object(state).at("definitions")).at("a"))
                    .at("card")));
  const auto meta = json::Object(draft.at("meta"));
  EXPECT_EQ(json::Text(meta, "name"), "new name");
  EXPECT_EQ(json::Text(meta, "description"), "Plots");
  const auto comment = json::Object(json::Array(meta.at("comments"))[0]);
  EXPECT_NE(json::Text(comment, "id"), "old");
  EXPECT_EQ(json::Text(comment, "text"), "one");
  EXPECT_EQ(json::Text(comment, "author"), "user");
  EXPECT_EQ(comment.at("resolved"), "true");
  ASSERT_EQ(edit.files.size(), 1);
  EXPECT_EQ(edit.files[0]->from, "definitions/a/src/asset.bin");
  EXPECT_EQ(edit.files[0]->to, "definitions/copy/src/asset.bin");
}

TEST(DocumentCopy, LinkedDefinitionCopiesNestedDocumentsAndAllOtherBytes) {
  const std::string state =
      R"({"definitions":{"a":{"card":{},"link":{"external":"inner"}}},"externals":{"externals/inner/":{"project":{"name":"inner","pkgsPath":"pinned"},"layout":{"cards":{"x":{"x":2}}},"definitions":{"x":{"card":{"template":"draft"},"meta":{"content":"unsaved"}}}},"externals/inner/externals/grand/":{"project":{"name":"grand"},"layout":{},"definitions":{}}}})";
  const std::vector<std::string> paths = {
      "externals/inner/tsugumi.json",
      "externals/inner/definitions/x/_meta.json",
      "externals/inner/definitions/removed/_card.json",
      "externals/inner/asset.bin",
      "externals/inner/externals/grand/data.bin",
      "externals/other/no"};
  const auto edit = CopyDocuments(state, paths, "definitions/a/",
                                  "externals/target/definitions/copy/", "");
  const auto patch = json::Object(edit.edits_json);
  const auto defs = json::Object(patch.at("definitions"));
  EXPECT_EQ(
      json::Text(json::Object(
                     json::Object(defs.at("externals/target/definitions/copy/"))
                         .at("link")),
                 "external"),
      "copy");
  const auto exts = json::Object(patch.at("externals"));
  const auto originals = json::Object(json::Object(state).at("externals"));
  EXPECT_EQ(exts.at("externals/target/externals/copy/"),
            originals.at("externals/inner/"));
  EXPECT_EQ(exts.at("externals/target/externals/copy/externals/grand/"),
            originals.at("externals/inner/externals/grand/"));
  ASSERT_EQ(edit.files.size(), 2);
  EXPECT_EQ(edit.files[0]->to, "externals/target/externals/copy/asset.bin");
  EXPECT_EQ(edit.files[1]->to,
            "externals/target/externals/copy/externals/grand/data.bin");
  const auto external =
      CopyDocuments(state, paths, "externals/inner/", "externals/another/", "");
  EXPECT_EQ(external.files.size(), 2);
  EXPECT_EQ(
      json::Object(json::Object(external.edits_json).at("externals")).size(),
      2);
}

TEST(DocumentCopy, MissingLinkedProjectCannotProduceAPartialCopy) {
  EXPECT_THROW(
      CopyDocuments(
          R"({"definitions":{"a":{"card":{},"link":{"external":"missing"}}}})",
          {}, "definitions/a/", "definitions/b/", ""),
      std::runtime_error);
  EXPECT_THROW(
      CopyDocuments("{}", {}, "externals/missing/", "externals/copy/", ""),
      std::runtime_error);
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
