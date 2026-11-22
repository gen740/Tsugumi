#include <gtest/gtest.h>

#include "scheduler.hh"
#include "store.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"

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
                     R"({"type":"trigger"})");
    fs::write_atomic(root / "layout.json", "{}");
  }
  auto TearDown() -> void override { std::filesystem::remove_all(root); }
  auto commit(Store& store, std::vector<ChangeIn> changes) -> DocumentPtr {
    auto staged = store.stage(changes, crypto::uuid4(), "agent");
    store.commit(staged, "save", "digest",
                 [](const Delta*) { return "receipt"; });
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
  put.json = R"({"type":"trigger","displayMode":{"future":true}})";
  put.meta_json = R"({"comments":[false],"unknown":12})";
  auto after = commit(store, {put});
  EXPECT_EQ(after->execution_rev, before->execution_rev);
  EXPECT_EQ(after->execution_fingerprint, before->execution_fingerprint);
  EXPECT_EQ(after->definitions.at("start").meta_json, put.meta_json);
  EXPECT_TRUE(after->resolution->nodes.front().diags.empty());
  EXPECT_TRUE(after->resolution->nodes.front().exec_diags.empty());
}
TEST_F(OpaqueMetadata, ExecutionFingerprintKeepsCanonicalBytes) {
  Document document;
  document.project_json =
      R"( { "name": "presentation", "value": {"z": [1, 1.0, -0, 2e+1], "\u0061": "\u0062\n"}, "agentExclude": [] } )";
  EXPECT_EQ(execution_fingerprint(document),
            crypto::sha256_hex(
                R"({"value":{"a":"b\n","z":[1,1.0,-0,2e+1]}})"));
}

TEST_F(OpaqueMetadata, ResolvedNixSourceKeepsCanonicalOverrideBytes) {
  fs::write_atomic(
      root / "tsugumi.json",
      R"({
        "pkgsPath": "tsugumi:default",
        "canvas": {"cards": [{"id": "start"}], "connections": []},
        "nixSources": [{
          "name": "science",
          "type": "flake",
          "ref": "github:org/science/revision?narHash=sha256-pin",
          "overrides": {"tool": {"override": {"z": [1.0, 1, null], "\u0061": "\u0062"}}}
        }]
      })");
  Store store({root, ""});
  store.open();
  const auto& sources = store.current()->resolution->nodes.front().nix_sources;
  ASSERT_EQ(sources.size(), 1u);
  EXPECT_EQ(sources.front().overrides,
            R"({"tool":{"override":{"a":"b","z":[1.0,1,null]}}})");
}

TEST_F(OpaqueMetadata, PortAndParamDiagnosticsNameWhatFixesThem) {
  // A wire to an undeclared input of a code Card and an unknown param type:
  // each message carries the fix (the inputs entry, the valid types) on one
  // line.
  fs::write_atomic(
      root / "tsugumi.json",
      R"({"pkgsPath":"tsugumi:default","canvas":{"cards":[{"id":"start"},{"id":"b"}],"connections":[{"from":{"card":"start","port":"out"},"to":{"card":"b","port":"config"}}]}})");
  fs::write_atomic(
      root / "definitions/b/_card.json",
      R"({"type":"code","params":[{"name":"x","type":"float","data":1}]})");
  Store store({root, ""});
  store.open();
  const auto doc = store.current();
  const auto* node = doc->resolution->find({"b"});
  ASSERT_NE(node, nullptr);
  bool port = false, param = false;
  for (const auto& d : node->diags) {
    port |= d.code == "unknown_port";
    param |= d.code == "invalid_param";
  }
  EXPECT_TRUE(port);
  EXPECT_TRUE(param);
}
TEST_F(OpaqueMetadata,
       AnEmptyEndOnAnImplementedCardAsksForAPortNotAnInputsEntry) {
  fs::write_atomic(
      root / "tsugumi.json",
      R"({"pkgsPath":"tsugumi:default","canvas":{"cards":[{"id":"start"},{"id":"b"}],"connections":[{"from":{"card":"start","port":"out"},"to":{"card":"b","port":""}}]}})");
  fs::write_atomic(root / "definitions/b/_card.json",
                   R"({"type":"code"})");
  Store store({root, ""});
  store.open();
  const auto* node = store.current()->resolution->find({"b"});
  ASSERT_NE(node, nullptr);
  bool port = false;
  for (const auto& d : node->diags)
    if (d.code == "unknown_port") {
      port = true;
      EXPECT_EQ(d.message.find("{\"name\": \"\"}"), std::string::npos)
          << d.message;
    }
  EXPECT_TRUE(port);
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
  card.json = R"({"type":"trigger","maxRuns":2})";
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
  card.json = R"({"displayMode":false, "type": "trigger"})";
  auto after = commit(store, {project, card});
  EXPECT_EQ(after->execution_fingerprint, before->execution_fingerprint);
  EXPECT_EQ(after->execution_rev, before->execution_rev);
}
TEST_F(OpaqueMetadata, MetadataMustStillBeABoundedJsonObject) {
  for (const auto& bad : {"[]", "null", "{", "{\"x\":NaN}"})
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
  fs::write_atomic(root / "definitions/outer/_card.json",
                   R"({"type":"subgraph","external":"library"})");
  const std::string project =
      R"({"canvas":{"cards":[{"id":"start"}],"connections":[]},"boundary":{"inputs":{},"outputs":{}}})";
  fs::write_atomic(root / "externals/library/tsugumi.json", project);
  fs::write_atomic(root / "externals/library/definitions/start/_card.json",
                   R"({"type":"trigger"})");
  fs::write_atomic(root / "externals/library/definitions/start/_meta.json",
                   R"({"name":false,"outputs":[false],"unknown":1})");
  Store store({root, ""});
  store.open();
  const auto before = store.current();
  auto put = [&](const std::string& path, const std::string& bytes) {
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
           R"({"type":"trigger","displayMode":false})"),
       put("externals/library/definitions/start/_meta.json",
           R"({"future":12,"comments":false})")});
  EXPECT_EQ(after->execution_fingerprint, before->execution_fingerprint);
  EXPECT_EQ(after->execution_rev, before->execution_rev);
  ASSERT_NE(after->resolution->find({"outer", "start"}), nullptr);
  EXPECT_TRUE(after->resolution->find({"outer", "start"})->diags.empty());
  after = commit(store, {put("externals/library/definitions/start/_card.json",
                             R"({"type":"trigger","maxRuns":2})")});
  EXPECT_NE(after->execution_fingerprint, before->execution_fingerprint);
}
TEST_F(OpaqueMetadata, DraftHasNoPortsFromMetadataOrSchema) {
  fs::write_atomic(root / "definitions/start/_card.json",
                   R"({"type":"draft"})");
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
  put.json = R"({"type":"draft"})";
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
                   R"({"type":"input"})");
  fs::write_atomic(root / "definitions/condition/_outputs.fbs",
                   "table Condition { value:bool; } table Outputs { "
                   "value:Condition; } root_type Outputs;");
  fs::write_atomic(root / "definitions/plan/_card.json",
                   R"({"type":"draft","inputs":[{"name":"ignored"}]})");
  fs::write_atomic(root / "definitions/next/_card.json",
                   R"({"type":"draft"})");
  fs::write_atomic(root / "definitions/branch/_card.json",
                   R"({"type":"branch"})");
  fs::write_atomic(root / "definitions/collect/_card.json",
                   R"({"type":"collect"})");
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

TEST_F(OpaqueMetadata, BranchConditionInheritsItsUpstreamSchema) {
  fs::write_atomic(
      root / "tsugumi.json",
      R"({"pkgsPath":"tsugumi:default","canvas":{"cards":[{"id":"start"},{"id":"plan"},{"id":"branch"}],"connections":[
    {"from":{"card":"start","port":"out"},"to":{"card":"branch","port":"condition"}},
    {"from":{"card":"plan","port":""},"to":{"card":"branch","port":"value"}}
  ]}})");
  fs::write_atomic(root / "definitions/plan/_card.json",
                   R"({"type":"draft"})");
  fs::write_atomic(root / "definitions/branch/_card.json",
                   R"({"type":"branch"})");
  Store store({root, ""});
  store.open();
  const auto& graph = *store.current()->resolution;
  const auto* branch = graph.find({"branch"});
  EXPECT_EQ(branch->input("condition")->type,
            graph.find({"start"})->output("out")->type);
  EXPECT_TRUE(branch->diags.empty());
}

TEST_F(OpaqueMetadata, ADraftAndARealSourceCannotShareAnInput) {
  fs::write_atomic(
      root / "tsugumi.json",
      R"({"pkgsPath":"tsugumi:default","canvas":{"cards":[{"id":"plan"},{"id":"sink"},{"id":"relay"},{"id":"condition"}],"connections":[
    {"from":{"card":"plan","port":""},"to":{"card":"sink","port":"value"}},
    {"from":{"card":"relay","port":"a"},"to":{"card":"sink","port":"value"}},
    {"from":{"card":"condition","port":"value"},"to":{"card":"relay","port":"value"}},
    {"from":{"card":"condition","port":"value"},"to":{"card":"relay","port":"condition"}},
    {"from":{"card":"condition","port":"value"},"to":{"card":"sink","port":"condition"}}
  ]}})");
  fs::write_atomic(root / "definitions/plan/_card.json",
                   R"({"type":"draft"})");
  fs::write_atomic(root / "definitions/sink/_card.json",
                   R"({"type":"branch"})");
  fs::write_atomic(root / "definitions/relay/_card.json",
                   R"({"type":"branch"})");
  fs::write_atomic(root / "definitions/condition/_card.json",
                   R"({"type":"input"})");
  fs::write_atomic(root / "definitions/condition/_outputs.fbs",
                   "table Condition { value:bool; } table Outputs { "
                   "value:Condition; } root_type Outputs;");
  Store store({root, ""});
  store.open();
  const auto* sink = store.current()->resolution->find({"sink"});
  EXPECT_EQ(sink->input("value")->type, types::empty_type());
  ASSERT_EQ(sink->diags.size(), 1);
  EXPECT_EQ(sink->diags.front().code, "multiple_input_wires");
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
                   R"({"type":"draft"})");
  fs::write_atomic(root / "definitions/next/_card.json",
                   R"({"type":"draft"})");
  Store store({root, ""});
  store.open();
  auto before = store.current();
  ChangeIn implementation{};
  implementation.kind = ChangeIn::Kind::PutDefinition;
  implementation.id = "plan";
  implementation.json = R"({"type":"collect"})";
  try {
    commit(store, {implementation});
    FAIL() << "Even the only input must be stated";
  } catch (const Failed& error) {
    EXPECT_EQ(error.failure.kind, FailureKind::validation_failed);
  }
  EXPECT_EQ(store.current(), before);
  EXPECT_EQ(fs::read_file(root / "definitions/plan/_card.json"),
            R"({"type":"draft"})");
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
    EXPECT_EQ(error.failure.kind, FailureKind::validation_failed);
  }
  const auto output = wiring.json.find(R"("card":"plan","port":"")");
  wiring.json.replace(output, std::string(R"("card":"plan","port":"")").size(),
                      R"("card":"plan","port":"out")");
  auto after = commit(store, {implementation, wiring});
  EXPECT_EQ(after->resolution->edges.size(), 2);
  EXPECT_EQ(after->resolution->find({"plan"})->kind, CardType::Collect);
  EXPECT_TRUE(after->resolution->find({"next"})->inputs.empty());
}

TEST_F(OpaqueMetadata, NestedImplementationRebindsBoundaryEndsAtomically) {
  fs::write_atomic(
      root / "tsugumi.json",
      R"({"pkgsPath":"tsugumi:default","canvas":{"cards":[{"id":"start"},{"id":"outer"},{"id":"next"}],"connections":[
    {"from":{"card":"start","port":"out"},"to":{"card":"outer","port":"in"}},
    {"from":{"card":"outer","port":"out"},"to":{"card":"next","port":""}}
  ]}})");
  fs::write_atomic(root / "definitions/outer/_card.json",
                   R"({"type":"subgraph","external":"child"})");
  fs::write_atomic(root / "definitions/next/_card.json",
                   R"({"type":"draft"})");
  fs::write_atomic(
      root / "externals/child/tsugumi.json",
      R"({"canvas":{"cards":[{"id":"plan"}],"connections":[]},"boundary":{"inputs":{"in":{"card":"plan","port":""}},"outputs":{"out":{"card":"plan","port":""}}}})");
  fs::write_atomic(root / "externals/child/definitions/plan/_card.json",
                   R"({"type":"draft"})");
  Store store({root, ""});
  store.open();
  ASSERT_EQ(store.current()->resolution->edges.size(), 2);
  ChangeIn implementation{};
  implementation.kind = ChangeIn::Kind::PutSource;
  implementation.path = "externals/child/definitions/plan/_card.json";
  implementation.base_hash =
      store.current()->sources.at(implementation.path).hash;
  implementation.bytes = R"({"type":"collect"})";
  try {
    commit(store, {implementation});
    FAIL();
  } catch (const Failed& error) {
    EXPECT_EQ(error.failure.kind, FailureKind::validation_failed);
  }
  EXPECT_EQ(store.current()->resolution->find({"outer", "plan"})->kind,
            CardType::Draft);
  ChangeIn wiring{};
  wiring.kind = ChangeIn::Kind::PutSource;
  wiring.path = "externals/child/tsugumi.json";
  wiring.base_hash = store.current()->sources.at(wiring.path).hash;
  wiring.bytes =
      R"({"canvas":{"cards":[{"id":"plan"}],"connections":[]},"boundary":{"inputs":{"in":{"card":"plan","port":"in"}},"outputs":{"out":{"card":"plan","port":"out"}}}})";
  const auto after = commit(store, {implementation, wiring});
  EXPECT_EQ(after->resolution->edges.size(), 2);
  EXPECT_EQ(after->resolution->find({"outer", "plan"})->kind,
            CardType::Collect);
}

TEST_F(OpaqueMetadata, NamedDraftEndIsRejectedAndIdentifiesTheWire) {
  fs::write_atomic(root / "definitions/plan/_card.json",
                   R"({"type":"draft"})");
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
    EXPECT_EQ(error.failure.kind, FailureKind::validation_failed);
  }
}
}  // namespace
}  // namespace tsugumi
