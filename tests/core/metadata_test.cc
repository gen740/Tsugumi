#include <gtest/gtest.h>

#include "scheduler.hh"
#include "store.hh"
#include "util/crypto.hh"
#include "util/fs.hh"

namespace tsugumi {
namespace {
class OpaqueMetadata : public ::testing::Test {
protected:
  std::filesystem::path root = std::filesystem::temp_directory_path() /
                               ("opaque-meta-" + crypto::uuid4());
  auto SetUp() -> void override {
    fs::write_atomic(
        root / "tsugumi.json",
        R"({"pkgsPath":"tsugumi:default","canvas":{"cards":[{"id":"start"}],"connections":[]}})");
    fs::write_atomic(root / "definitions/start/_card.json",
                     R"({"template":"trigger"})");
    fs::write_atomic(root / "layout.json", "{}");
  }
  auto TearDown() -> void override { std::filesystem::remove_all(root); }
  auto commit(Store &store, std::vector<ChangeIn> changes) -> DocumentPtr {
    auto staged = store.stage(changes, crypto::uuid4(), "agent");
    store.commit(staged, "save", "digest",
                 [](const Delta *) { return "receipt"; });
    return store.current();
  }
};
TEST_F(OpaqueMetadata,
       UnknownAndMalformedPresentationFieldsSurviveLoadSaveAndResolution) {
  const std::string meta =
      R"({"name":[],"description":false,"comments":"not a list","outputs":false,"future":{"x":1}})";
  fs::write_atomic(root / "definitions/start/_meta.json", meta);
  Store store({root, ""});
  store.open();
  auto before = store.current();
  ASSERT_EQ(before->definitions.at("start").meta_json, meta);
  ASSERT_TRUE(before->resolution->nodes.front().diags.empty());
  ChangeIn put{};
  put.kind = ChangeIn::Kind::PutDefinition;
  put.id = "start";
  put.json = R"({"template":"trigger","displayMode":{"future":true}})";
  put.meta_json = R"({"comments":[false],"unknown":12})";
  auto after = commit(store, {put});
  EXPECT_EQ(after->execution_rev, before->execution_rev);
  EXPECT_EQ(after->execution_fingerprint, before->execution_fingerprint);
  EXPECT_EQ(after->definitions.at("start").meta_json, put.meta_json);
  EXPECT_TRUE(after->resolution->nodes.front().diags.empty());
  EXPECT_TRUE(after->resolution->nodes.front().exec_diags.empty());
}
TEST_F(OpaqueMetadata, PortAndParamDiagnosticsNameWhatFixesThem) {
  // A wire to an undeclared input of a code Card and an unknown param type: each
  // message carries the fix (the inputs entry, the valid types) on one line.
  fs::write_atomic(
      root / "tsugumi.json",
      R"({"pkgsPath":"tsugumi:default","canvas":{"cards":[{"id":"start"},{"id":"b"}],"connections":[{"from":{"card":"start","port":"out"},"to":{"card":"b","port":"config"}}]}})");
  fs::write_atomic(root / "definitions/b/_card.json",
                   R"({"language":"python","params":[{"name":"x","type":"float","data":1}]})");
  Store store({root, ""});
  store.open();
  const auto doc = store.current();
  const auto* node = doc->resolution->find({"b"});
  ASSERT_NE(node, nullptr);
  bool port = false, param = false;
  for (const auto& d : node->diags) {
    EXPECT_EQ(d.message.find('\n'), std::string::npos);
    if (d.code == "unknown_port") {
      port = true;
      EXPECT_NE(d.message.find("{\"name\": \"config\"}"), std::string::npos) << d.message;
      EXPECT_NE(d.message.find("\"inputs\""), std::string::npos) << d.message;
    }
    if (d.code == "invalid_param") {
      param = true;
      for (const auto* type : {"bool", "int64", "uint64", "double", "string", "[]"})
        EXPECT_NE(d.message.find(type), std::string::npos) << d.message;
    }
  }
  EXPECT_TRUE(port);
  EXPECT_TRUE(param);
}
TEST_F(OpaqueMetadata, ProjectNameIsOptionalOpaqueAndDoesNotChangeExecution) {
  Store store({root, ""});
  store.open();
  auto before = store.current();
  ChangeIn project{};
  project.kind = ChangeIn::Kind::SetProject;
  project.json =
      R"({"name":{"future":"anything"},"pkgsPath":"tsugumi:default","canvas":{"cards":[{"id":"start"}],"connections":[]}})";
  auto after = commit(store, {project});
  EXPECT_EQ(after->execution_rev, before->execution_rev);
  EXPECT_EQ(after->execution_fingerprint, before->execution_fingerprint);
  EXPECT_EQ(after->project_json, project.json);
  ChangeIn card{};
  card.kind = ChangeIn::Kind::PutDefinition;
  card.id = "start";
  card.json = R"({"template":"trigger","maxRuns":2})";
  after = commit(store, {card});
  EXPECT_NE(after->execution_fingerprint, before->execution_fingerprint);
  EXPECT_GT(after->execution_rev, before->execution_rev);
}
TEST_F(OpaqueMetadata,
       ClientReformattingAlongWithPresentationKeepsTheSameFingerprint) {
  Store store({root, ""});
  store.open();
  auto before = store.current();
  ChangeIn project{};
  project.kind = ChangeIn::Kind::SetProject;
  project.json =
      R"({ "canvas": { "connections": [ ], "cards": [ { "id": "start" } ] }, "name": "Renamed", "pkgsPath": "tsugumi:default" })";
  ChangeIn card{};
  card.kind = ChangeIn::Kind::PutDefinition;
  card.id = "start";
  card.json = R"({"displayMode":false, "template": "trigger"})";
  auto after = commit(store, {project, card});
  EXPECT_EQ(after->execution_fingerprint, before->execution_fingerprint);
  EXPECT_EQ(after->execution_rev, before->execution_rev);
}
TEST_F(OpaqueMetadata, MetadataMustStillBeABoundedJsonObject) {
  for (const auto &bad : {"[]", "null", "{", "{\"x\":NaN}"})
    EXPECT_THROW(check_meta_json(bad), DocumentError);
  EXPECT_THROW(
      check_meta_json("{\"x\":\"" + std::string(1u << 20, 'a') + "\"}"),
      DocumentError);
}
TEST_F(OpaqueMetadata, ExternalPresentationUsesTheSameBoundaryAndPendingBytes) {
  fs::write_atomic(
      root / "tsugumi.json",
      R"({"pkgsPath":"tsugumi:default","canvas":{"cards":[{"id":"outer"}],"connections":[]}})");
  std::filesystem::remove_all(root / "definitions/start");
  fs::write_atomic(root / "definitions/outer/_link.json",
                   R"({"external":"library"})");
  const std::string project =
      R"({"canvas":{"cards":[{"id":"start"}],"connections":[]},"boundary":{"inputs":{},"outputs":{}}})";
  fs::write_atomic(root / "externals/library/tsugumi.json", project);
  fs::write_atomic(root / "externals/library/definitions/start/_card.json",
                   R"({"template":"trigger"})");
  fs::write_atomic(root / "externals/library/definitions/start/_meta.json",
                   R"({"name":false,"outputs":[false],"unknown":1})");
  Store store({root, ""});
  store.open();
  const auto before = store.current();
  auto put = [&](const std::string &path, const std::string &bytes) {
    ChangeIn c{};
    c.kind = ChangeIn::Kind::PutSource;
    c.path = path;
    c.bytes = bytes;
    c.base_hash = store.current()->sources.at(path).hash;
    return c;
  };
  auto after = commit(
      store,
      {put("externals/library/tsugumi.json",
           R"({"name":false,"canvas":{"cards":[{"id":"start"}],"connections":[]},"boundary":{"inputs":{},"outputs":{}}})"),
       put("externals/library/definitions/start/_card.json",
           R"({"template":"trigger","displayMode":false})"),
       put("externals/library/definitions/start/_meta.json",
           R"({"future":12,"comments":false})")});
  EXPECT_EQ(after->execution_fingerprint, before->execution_fingerprint);
  EXPECT_EQ(after->execution_rev, before->execution_rev);
  ASSERT_NE(after->resolution->find({"outer", "start"}), nullptr);
  EXPECT_TRUE(after->resolution->find({"outer", "start"})->diags.empty());
  after = commit(store, {put("externals/library/definitions/start/_card.json",
                             R"({"template":"trigger","maxRuns":2})")});
  EXPECT_NE(after->execution_fingerprint, before->execution_fingerprint);
}
TEST_F(OpaqueMetadata, DraftHasNoPortsFromMetadataOrSchema) {
  fs::write_atomic(root / "definitions/start/_card.json",
                   R"({"template":"draft"})");
  fs::write_atomic(
      root / "definitions/start/_meta.json",
      R"({"name":false,"comments":12,"outputs":[{"name":"planned","description":false,"future":1},false]})");
  fs::write_atomic(root / "definitions/start/_outputs.fbs", "not a schema");
  Store store({root, ""});
  store.open();
  const auto* draft = store.current()->resolution->find({"start"});
  EXPECT_TRUE(draft->inputs.empty());
  EXPECT_TRUE(draft->outputs.empty());
  EXPECT_TRUE(draft->diags.empty());
  auto before = store.current();
  ChangeIn put{};
  put.kind = ChangeIn::Kind::PutDefinition;
  put.id = "start";
  put.json = R"({"template":"draft"})";
  put.meta_json = R"({"outputs":[{"name":"different"}]})";
  auto after = commit(store, {put});
  EXPECT_EQ(after->resolution, before->resolution);
}

TEST_F(OpaqueMetadata, DraftEndsAcceptManyWiresAndCarryEmptyWithoutPorts) {
  fs::write_atomic(
      root / "tsugumi.json",
      R"({"pkgsPath":"tsugumi:default","canvas":{"cards":[{"id":"start"},{"id":"condition"},{"id":"plan"},{"id":"next"},{"id":"branch"},{"id":"collect"}],"connections":[
    {"from":{"card":"start","port":"out"},"to":{"card":"plan","port":""}},
    {"from":{"card":"condition","port":"value"},"to":{"card":"plan","port":""}},
    {"from":{"card":"plan","port":""},"to":{"card":"next","port":""}},
    {"from":{"card":"plan","port":""},"to":{"card":"branch","port":"condition"}},
    {"from":{"card":"plan","port":""},"to":{"card":"branch","port":"value"}},
    {"from":{"card":"next","port":""},"to":{"card":"collect","port":"in"}}
  ]}})");
  fs::write_atomic(root / "definitions/condition/_card.json",
                   R"({"template":"switch"})");
  fs::write_atomic(root / "definitions/plan/_card.json",
                   R"({"template":"draft","inputs":[{"name":"ignored"}]})");
  fs::write_atomic(root / "definitions/next/_card.json",
                   R"({"template":"draft"})");
  fs::write_atomic(root / "definitions/branch/_card.json",
                   R"({"template":"branch"})");
  fs::write_atomic(root / "definitions/collect/_card.json",
                   R"({"template":"collect"})");
  Store store({root, ""});
  store.open();
  const auto& resolution = *store.current()->resolution;
  EXPECT_EQ(resolution.edges.size(), 6);
  const auto waiting = Scheduler::draft_waiting(resolution);
  for (const auto* id : {"plan", "next", "branch", "collect"})
    EXPECT_TRUE(waiting.contains(resolution.index.at(id)));
  EXPECT_FALSE(waiting.contains(resolution.index.at("start")));
  EXPECT_TRUE(resolution.find({"plan"})->inputs.empty());
  EXPECT_TRUE(resolution.find({"plan"})->outputs.empty());
  EXPECT_TRUE(resolution.find({"next"})->inputs.empty());
  EXPECT_EQ(resolution.find({"branch"})->input("condition")->type,
            types::empty_type());
  for (const auto& diagnostic : resolution.all_diagnostics(false))
    ADD_FAILURE() << diagnostic.code << ": " << diagnostic.message;
}

TEST_F(OpaqueMetadata, RealEmptyConditionIsStillInvalidBesideADraft) {
  fs::write_atomic(
      root / "tsugumi.json",
      R"({"pkgsPath":"tsugumi:default","canvas":{"cards":[{"id":"start"},{"id":"plan"},{"id":"branch"}],"connections":[
    {"from":{"card":"start","port":"out"},"to":{"card":"branch","port":"condition"}},
    {"from":{"card":"plan","port":""},"to":{"card":"branch","port":"value"}}
  ]}})");
  fs::write_atomic(root / "definitions/plan/_card.json",
                   R"({"template":"draft"})");
  fs::write_atomic(root / "definitions/branch/_card.json",
                   R"({"template":"branch"})");
  Store store({root, ""});
  store.open();
  const auto& diagnostics =
      store.current()->resolution->find({"branch"})->diags;
  ASSERT_EQ(diagnostics.size(), 1);
  EXPECT_EQ(diagnostics.front().code, "invalid_condition");
}

TEST_F(OpaqueMetadata, DraftEmptyCannotMaskARealTypeDerivedSeveralStepsLater) {
  fs::write_atomic(
      root / "tsugumi.json",
      R"({"pkgsPath":"tsugumi:default","canvas":{"cards":[{"id":"plan"},{"id":"sink"},{"id":"relay"},{"id":"condition"}],"connections":[
    {"from":{"card":"plan","port":""},"to":{"card":"sink","port":"in"}},
    {"from":{"card":"relay","port":"out1"},"to":{"card":"sink","port":"in"}},
    {"from":{"card":"condition","port":"value"},"to":{"card":"relay","port":"in"}}
  ]}})");
  fs::write_atomic(root / "definitions/plan/_card.json",
                   R"({"template":"draft"})");
  fs::write_atomic(root / "definitions/sink/_card.json",
                   R"({"template":"fork"})");
  fs::write_atomic(root / "definitions/relay/_card.json",
                   R"({"template":"fork"})");
  fs::write_atomic(root / "definitions/condition/_card.json",
                   R"({"template":"switch"})");
  Store store({root, ""});
  store.open();
  const auto* sink = store.current()->resolution->find({"sink"});
  EXPECT_EQ(sink->input("in")->type, types::bool_condition_type());
  EXPECT_EQ(sink->output("out1")->type, types::bool_condition_type());
  EXPECT_TRUE(sink->diags.empty());
}

TEST_F(OpaqueMetadata,
       ImplementingRequiresExplicitInputAndOutputInTheSameSave) {
  const std::string project =
      R"({"pkgsPath":"tsugumi:default","canvas":{"cards":[{"id":"start"},{"id":"plan"},{"id":"next"}],"connections":[
    {"from":{"card":"start","port":"out"},"to":{"card":"plan","port":""}},
    {"from":{"card":"plan","port":""},"to":{"card":"next","port":""}}
  ]}})";
  fs::write_atomic(root / "tsugumi.json", project);
  fs::write_atomic(root / "definitions/plan/_card.json",
                   R"({"template":"draft"})");
  fs::write_atomic(root / "definitions/next/_card.json",
                   R"({"template":"draft"})");
  Store store({root, ""});
  store.open();
  auto before = store.current();
  ChangeIn implementation{};
  implementation.kind = ChangeIn::Kind::PutDefinition;
  implementation.id = "plan";
  implementation.json = R"({"template":"fork"})";
  try {
    commit(store, {implementation});
    FAIL() << "Even the only input must be stated";
  } catch (const Failed& error) {
    EXPECT_NE(std::string(error.what()).find("wire start.out -> plan."),
              std::string::npos);
  }
  EXPECT_EQ(store.current(), before);
  EXPECT_EQ(fs::read_file(root / "definitions/plan/_card.json"),
            R"({"template":"draft"})");
  ChangeIn wiring{};
  wiring.kind = ChangeIn::Kind::SetProject;
  wiring.json = project;
  const auto input = wiring.json.find(R"("card":"plan","port":"")");
  wiring.json.replace(input, std::string(R"("card":"plan","port":"")").size(),
                      R"("card":"plan","port":"in")");
  try {
    commit(store, {implementation, wiring});
    FAIL() << "The output must also be stated";
  } catch (const Failed& error) {
    EXPECT_NE(std::string(error.what()).find("wire plan. -> next."),
              std::string::npos);
  }
  const auto output = wiring.json.find(R"("card":"plan","port":"")");
  wiring.json.replace(output, std::string(R"("card":"plan","port":"")").size(),
                      R"("card":"plan","port":"out1")");
  auto after = commit(store, {implementation, wiring});
  EXPECT_EQ(after->resolution->edges.size(), 2);
  EXPECT_EQ(after->resolution->find({"plan"})->kind, Template::Fork);
  EXPECT_TRUE(after->resolution->find({"next"})->inputs.empty());
}

TEST_F(OpaqueMetadata, NestedImplementationRebindsBoundaryEndsAtomically) {
  fs::write_atomic(
      root / "tsugumi.json",
      R"({"pkgsPath":"tsugumi:default","canvas":{"cards":[{"id":"start"},{"id":"outer"},{"id":"next"}],"connections":[
    {"from":{"card":"start","port":"out"},"to":{"card":"outer","port":"in"}},
    {"from":{"card":"outer","port":"out"},"to":{"card":"next","port":""}}
  ]}})");
  fs::write_atomic(root / "definitions/outer/_link.json",
                   R"({"external":"child"})");
  fs::write_atomic(root / "definitions/next/_card.json",
                   R"({"template":"draft"})");
  fs::write_atomic(
      root / "externals/child/tsugumi.json",
      R"({"canvas":{"cards":[{"id":"plan"}],"connections":[]},"boundary":{"inputs":{"in":{"card":"plan","port":""}},"outputs":{"out":{"card":"plan","port":""}}}})");
  fs::write_atomic(root / "externals/child/definitions/plan/_card.json",
                   R"({"template":"draft"})");
  Store store({root, ""});
  store.open();
  ASSERT_EQ(store.current()->resolution->edges.size(), 2);
  ChangeIn implementation{};
  implementation.kind = ChangeIn::Kind::PutSource;
  implementation.path = "externals/child/definitions/plan/_card.json";
  implementation.base_hash =
      store.current()->sources.at(implementation.path).hash;
  implementation.bytes = R"({"template":"fork"})";
  try {
    commit(store, {implementation});
    FAIL();
  } catch (const Failed& error) {
    EXPECT_NE(std::string(error.what()).find("wire start.out -> outer.in"),
              std::string::npos);
  }
  EXPECT_EQ(store.current()->resolution->find({"outer", "plan"})->kind,
            Template::Draft);
  ChangeIn wiring{};
  wiring.kind = ChangeIn::Kind::PutSource;
  wiring.path = "externals/child/tsugumi.json";
  wiring.base_hash = store.current()->sources.at(wiring.path).hash;
  wiring.bytes =
      R"({"canvas":{"cards":[{"id":"plan"}],"connections":[]},"boundary":{"inputs":{"in":{"card":"plan","port":"in"}},"outputs":{"out":{"card":"plan","port":"out1"}}}})";
  const auto after = commit(store, {implementation, wiring});
  EXPECT_EQ(after->resolution->edges.size(), 2);
  EXPECT_EQ(after->resolution->find({"outer", "plan"})->kind, Template::Fork);
}

TEST_F(OpaqueMetadata, NamedDraftEndIsRejectedAndIdentifiesTheWire) {
  fs::write_atomic(root / "definitions/plan/_card.json",
                   R"({"template":"draft"})");
  Store store({root, ""});
  store.open();
  ChangeIn wiring{};
  wiring.kind = ChangeIn::Kind::SetProject;
  wiring.json =
      R"({"pkgsPath":"tsugumi:default","canvas":{"cards":[{"id":"start"},{"id":"plan"}],"connections":[{"from":{"card":"start","port":"out"},"to":{"card":"plan","port":"in"}}]}})";
  try {
    commit(store, {wiring});
    FAIL();
  } catch (const Failed& error) {
    EXPECT_NE(std::string(error.what()).find("wire start.out -> plan.in"),
              std::string::npos);
  }
}
} // namespace
} // namespace tsugumi
