#include <gtest/gtest.h>

#include "nix_params.hh"
#include "scheduler.hh"
#include "store.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/util/json_object.hh"

namespace tsugumi {
namespace {
class DocumentStorage : public ::testing::Test {
 protected:
  std::filesystem::path root = std::filesystem::temp_directory_path() /
                               ("document-storage-" + crypto::uuid4());
  auto SetUp() -> void override {
    fs::write_atomic(
        root / "tsugumi.json",
        R"({"nixPath": [],"canvas":{"cards":[{"id":"start"}],"connections":[]}})");
    fs::write_atomic(root / "definitions/start/_card.json",
                     R"({"type":"trigger"})");
  }
  auto TearDown() -> void override { std::filesystem::remove_all(root); }
  auto commit(Store& store, std::vector<ChangeIn> changes) -> DocumentPtr {
    auto staged = store.stage(changes, crypto::uuid4(), "agent");
    store.commit(staged, "save", "digest",
                 [](const Delta*) { return "receipt"; });
    return store.current();
  }
};
TEST_F(DocumentStorage, EditingReadmeAndCommentsKeepsExecutionIdentity) {
  const std::string readme = "---\ndescription: Start the run\n---\n";
  fs::write_atomic(root / "definitions/start/README.md", readme);
  Store store({root, ""});
  store.open();
  const auto before = store.current();
  ASSERT_EQ(before->definitions.at("start").readme, readme);
  ChangeIn put{};
  put.kind = ChangeIn::Kind::PutDefinition;
  put.id = "start";
  put.json = R"({"type":"trigger"})";
  put.readme = readme + "\nRun notes.\n";
  put.comments_json = R"({"comments":[{"id":"one","text":"Review this."}]})";
  const auto after = commit(store, {put});
  EXPECT_EQ(after->resolution, before->resolution);
  EXPECT_EQ(after->execution_rev, before->execution_rev);
  EXPECT_EQ(after->definitions.at("start").readme, put.readme);
  EXPECT_EQ(after->definitions.at("start").comments_json,
            parse_comments_json("_comments.json", *put.comments_json).text);
}

TEST_F(DocumentStorage, CanonicalTextPreservesWhitespaceInsideStrings) {
  const auto parsed = parse_project_json(
      R"({"name":"[ ] { } [\" ]","canvas":{"connections":[],"cards":[]},"nixPath": []})");
  EXPECT_EQ(parse_project_json(parsed.text).text, parsed.text);
  EXPECT_EQ(document_text::root<Tsugumi::Document::Project>(parsed.buffer).name()->str(),
            R"([ ] { } [" ])");
}

TEST_F(DocumentStorage, ResolvedNixPathUsesTheCardsProject) {
  fs::write_atomic(
      root / "tsugumi.json",
      R"({"canvas":{"cards":[{"id":"start"}],"connections":[]},"nixPath":[{"name":"science","url":"github:org/science"}]})");
  Store store({root, ""});
  store.open();
  const auto& sources = store.current()->resolution->nodes.front().nix_path;
  ASSERT_EQ(sources.size(), 1u);
  EXPECT_EQ(sources.front().name, "science");
  EXPECT_EQ(sources.front().url, "github:org/science");
}

TEST(NixParams, AcceptsTypedLiteralsAndSortsByName) {
  const auto actual = evaluate_literal_params("definitions/a/", R"nix(
    {
      z = -3;
      zero = 0.0;
      int = 10;
      float = 10.0;
      exp = 1e3;
      negative = -0.5;
      flag = true;
      text = "quoted\ntext";
      indented = ''indented'';
      ints = [ 1 (-2) ];
      floats = [ 1.0 (-2.0) ];
      bools = [ false true ];
      strings = [ "" "x" ];
      flag2 = false;
    }
  )nix");
  EXPECT_EQ(
      actual,
      R"([{"name":"bools","type":"[bool]","value":[false,true]},{"name":"exp","type":"float","value":1000.0},{"name":"flag","type":"bool","value":true},{"name":"flag2","type":"bool","value":false},{"name":"float","type":"float","value":10.0},{"name":"floats","type":"[float]","value":[1.0,-2.0]},{"name":"indented","type":"string","value":"indented"},{"name":"int","type":"int","value":10},{"name":"ints","type":"[int]","value":[1,-2]},{"name":"negative","type":"float","value":-0.5},{"name":"strings","type":"[string]","value":["","x"]},{"name":"text","type":"string","value":"quoted\ntext"},{"name":"z","type":"int","value":-3},{"name":"zero","type":"float","value":0.0}])");
  EXPECT_EQ(evaluate_literal_params("definitions/a/",
                                    "{ min = -9223372036854775808; }"),
            R"([{"name":"min","type":"int","value":-9223372036854775808}])");
  EXPECT_EQ(evaluate_literal_params("definitions/a/", "{ }"), R"([])");
}

TEST(NixParams, RejectsExpressionsAndInvalidDeclarations) {
  for (const auto* source : {
           "{ a = []; }",
           "{ a = [ 1 2.0 ]; }",
           "{ a = [[1]]; }",
           "{ a = { x = 1; }; }",
           "{ a = null; }",
           "{ a = ./data; }",
           "{ a = import ./data; }",
           "{ a = __sub 0 1; }",
           "{ a = 0 - 1; }",
           "{ a = 1 + 2; }",
           "{ a = \"$"
           "{true}\"; }",
           "{ a = ''$"
           "{\"x\"}''; }",
           "{ $"
           "{\"a\"} = 0; }",
           "{ run = let x = 1; in { a = x; }; }",
           "{ run = with builtins; { a = 0; }; }",
           "{ run = rec { a = 0; }; }",
           "{ inherit (builtins) true; }",
           "{ inherit; }",
           "{ a = 0; a = 1; }",
           "{ a = 0; }; build = { a = 1; }",
           "{ runtime = {}; }",
           "{ \"a/b\" = 1; }",
       }) {
    SCOPED_TRACE(source);
    try {
      evaluate_literal_params("definitions/a/", source);
      FAIL() << "accepted a nonliteral or invalid declaration";
    } catch (const std::exception& error) {
      EXPECT_TRUE(std::string_view(error.what())
                      .starts_with("definitions/a/params.nix:"));
    }
  }
}

TEST(NixParams, RewriteKeepsCommentsInsideAList) {
  const std::string source = "{ items = [ 1 /* keep */ 2 # line\n 3 ]; }";
  const auto edited =
      rewrite_param_literal("params.nix", source, "items", "[ 7 ]");
  EXPECT_EQ(edited, "{ items = [ 7 ]  /* keep */  # line\n  ; }");
  EXPECT_EQ(evaluate_literal_params("", edited),
            R"([{"name":"items","type":"[int]","value":[7]}])");
}

TEST_F(DocumentStorage, AllParamsAreEncodedForNixAndTheCard) {
  fs::write_atomic(
      root / "tsugumi.json",
      R"({"canvas":{"cards":[{"id":"start"},{"id":"a"}],"connections":[]}})");
  fs::write_atomic(root / "definitions/a/_card.json", R"({"type":"code"})");
  fs::write_atomic(root / "definitions/a/params.nix",
                   "{ z = 0.0; a = false; count = -5; preset = 5.0; }");
  Store store({root, ""});
  store.open();
  const auto* node = store.current()->resolution->find({"a"});
  ASSERT_NE(node, nullptr);
  EXPECT_TRUE(node->diags.empty());
  EXPECT_EQ(node->params_nix_json,
            R"({"a":false,"count":-5,"preset":5.0,"z":0.0})");
  const auto buffer = params_buffer(node->settings.params);
  const auto* table = flatbuffers::GetRoot<flatbuffers::Table>(buffer.data());
  const auto field = [](int id) { return flatbuffers::FieldIndexToOffset(id); };
  EXPECT_TRUE(table->CheckField(field(0)));
  EXPECT_FALSE(table->GetField<uint8_t>(field(0), 1));
  EXPECT_EQ(table->GetField<int64_t>(field(1), 0), -5);
  EXPECT_DOUBLE_EQ(table->GetField<double>(field(2), 1.0), 5.0);
  EXPECT_TRUE(table->CheckField(field(3)));
  EXPECT_DOUBLE_EQ(table->GetField<double>(field(3), 1.0), 0.0);
  EXPECT_EQ(normalize_param_value(ParamType::Int64, "9.223372036854775807e18"),
            "9223372036854775807");
}

TEST_F(DocumentStorage, LockIsCarriedAndConflictsWithDiskEdits) {
  const std::string lock =
      R"({"entries":[{"name":"source","url":"github:org/repo","rev":"0123456789012345678901234567890123456789","narHash":"sha256-123"}]})";
  Store store({root, ""});
  store.open();
  EXPECT_FALSE(store.current()->sources.contains("tsugumi.lock"));
  ChangeIn change{};
  change.kind = ChangeIn::Kind::SetProjectLock;
  change.path = "tsugumi.lock";
  change.lock_json = parse_project_lock(change.path, lock).text;
  const auto after = commit(store, {change});
  EXPECT_EQ(*after->sources.at(change.path).document, *change.lock_json);
  EXPECT_EQ(fs::read_file(root / change.path), change.lock_json);
  change.base_hash = after->sources.at(change.path).hash;
  fs::write_atomic(root / change.path, lock);
  EXPECT_THROW(store.stage({change}, crypto::uuid4(), "ui"), Failed);
}

TEST_F(DocumentStorage, PortAndParamDiagnosticsNameWhatFixesThem) {
  // A wire to an undeclared input of a code Card and an unknown param type:
  // each message carries the fix (the inputs entry, the valid types) on one
  // line.
  fs::write_atomic(
      root / "tsugumi.json",
      R"({"nixPath": [],"canvas":{"cards":[{"id":"start"},{"id":"b"}],"connections":[{"from":{"card":"start","port":"out"},"to":{"card":"b","port":"config"}}]}})");
  fs::write_atomic(root / "definitions/b/_card.json", R"({"type":"code"})");
  fs::write_atomic(root / "definitions/b/params.nix", "{ x = null; }");
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
TEST_F(DocumentStorage,
       AnEmptyEndOnAnImplementedCardAsksForAPortNotAnInputsEntry) {
  fs::write_atomic(
      root / "tsugumi.json",
      R"({"nixPath": [],"canvas":{"cards":[{"id":"start"},{"id":"b"}],"connections":[{"from":{"card":"start","port":"out"},"to":{"card":"b","port":""}}]}})");
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
TEST_F(DocumentStorage, ProjectNameDoesNotChangeExecution) {
  Store store({root, ""});
  store.open();
  auto before = store.current();
  ChangeIn project{};
  project.kind = ChangeIn::Kind::SetProject;
  project.json =
      R"({"name":"Renamed","nixPath": [],"canvas":{"cards":[{"id":"start"}],"connections":[]}})";
  auto after = commit(store, {project});
  EXPECT_EQ(after->execution_rev, before->execution_rev);
  EXPECT_EQ(after->execution_fingerprint, before->execution_fingerprint);
  EXPECT_EQ(after->project_json, parse_project_json(project.json).text);
  ChangeIn card{};
  card.kind = ChangeIn::Kind::PutDefinition;
  card.id = "start";
  card.json = R"({"type":"trigger","maxRuns":2})";
  after = commit(store, {card});
  EXPECT_NE(after->execution_fingerprint, before->execution_fingerprint);
  EXPECT_GT(after->execution_rev, before->execution_rev);
}
TEST_F(DocumentStorage,
       ClientReformattingAlongWithPresentationKeepsTheSameFingerprint) {
  Store store({root, ""});
  store.open();
  auto before = store.current();
  ChangeIn project{};
  project.kind = ChangeIn::Kind::SetProject;
  project.json =
      R"({ "canvas": { "connections": [ ], "cards": [ { "id": "start" } ] }, "name": "Renamed", "nixPath": [] })";
  ChangeIn card{};
  card.kind = ChangeIn::Kind::PutDefinition;
  card.id = "start";
  card.json = R"({"displayMode":"live", "type": "trigger"})";
  auto after = commit(store, {project, card});
  EXPECT_EQ(after->execution_fingerprint, before->execution_fingerprint);
  EXPECT_EQ(after->execution_rev, before->execution_rev);
}
TEST_F(DocumentStorage, ReadmeAndCommentsValidateTheirFormats) {
  EXPECT_THROW(check_readme("README.md", "---\ntitle: Old title\n---\n"),
               DocumentError);
  for (const auto& bad : {"[]", "null", "{", "{\"comments\":[false]}"})
    EXPECT_THROW(parse_comments_json("_comments.json", bad), DocumentError);
}

TEST_F(DocumentStorage, ExternalPresentationUsesTheSameBoundaryAndPendingBytes) {
  fs::write_atomic(
      root / "tsugumi.json",
      R"({"nixPath": [],"canvas":{"cards":[{"id":"outer"}],"connections":[]}})");
  std::filesystem::remove_all(root / "definitions/start");
  fs::write_atomic(root / "definitions/outer/_card.json",
                   R"({"type":"subgraph","external":"library"})");
  const std::string project =
      R"({"canvas":{"cards":[{"id":"start"}],"connections":[]},"boundary":{"inputs":[],"outputs":[]}})";
  fs::write_atomic(root / "externals/library/tsugumi.json", project);
  fs::write_atomic(root / "externals/library/definitions/start/_card.json",
                   R"({"type":"trigger"})");
  fs::write_atomic(root / "externals/library/definitions/start/README.md",
                   "Original description");
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
           R"({"name":"Renamed","canvas":{"cards":[{"id":"start"}],"connections":[]},"boundary":{"inputs":[],"outputs":[]}})"),
       put("externals/library/definitions/start/_card.json",
           R"({"type":"trigger","displayMode":"live"})"),
       put("externals/library/definitions/start/README.md",
           "Updated description")});
  EXPECT_EQ(after->execution_fingerprint, before->execution_fingerprint);
  EXPECT_EQ(after->execution_rev, before->execution_rev);
  ASSERT_NE(after->resolution->find({"outer", "start"}), nullptr);
  EXPECT_TRUE(after->resolution->find({"outer", "start"})->diags.empty());
  after = commit(store, {put("externals/library/definitions/start/_card.json",
                             R"({"type":"trigger","maxRuns":2})")});
  EXPECT_NE(after->execution_fingerprint, before->execution_fingerprint);
}
TEST_F(DocumentStorage, DraftHasNoPortsFromMetadataOrSchema) {
  fs::write_atomic(root / "definitions/start/_card.json",
                   R"({"type":"draft"})");
  fs::write_atomic(
      root / "definitions/start/README.md",
      "A future port called planned.");
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
  put.readme = "A future port called different.";
  auto after = commit(store, {put});
  EXPECT_EQ(after->resolution, before->resolution);
}

TEST_F(DocumentStorage, DraftEndsAcceptManyWiresAndCarryEmptyWithoutPorts) {
  fs::write_atomic(
      root / "tsugumi.json",
      R"({"nixPath": [],"canvas":{"cards":[{"id":"start"},{"id":"condition"},{"id":"plan"},{"id":"next"},{"id":"branch"},{"id":"collect"}],"connections":[
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

TEST_F(DocumentStorage, BranchConditionInheritsItsUpstreamSchema) {
  fs::write_atomic(
      root / "tsugumi.json",
      R"({"nixPath": [],"canvas":{"cards":[{"id":"start"},{"id":"plan"},{"id":"branch"}],"connections":[
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

TEST_F(DocumentStorage, NixPortsCarryWiredTypesAndOnlyAllowedExports) {
  fs::write_atomic(root / "tsugumi.json", R"({
    "canvas": {
      "cards": [{"id":"start"},{"id":"producer"},{"id":"branch"},{"id":"fork"},
                {"id":"collect"},{"id":"merge"},{"id":"draft"},{"id":"consumer"}],
      "connections": [
        {"from":{"card":"producer","port":"out"},"to":{"card":"consumer","port":"direct"}},
        {"from":{"card":"producer","port":"out"},"to":{"card":"branch","port":"value"}},
        {"from":{"card":"start","port":"out"},"to":{"card":"branch","port":"condition"}},
        {"from":{"card":"branch","port":"a"},"to":{"card":"consumer","port":"branch"}},
        {"from":{"card":"branch","port":"a"},"to":{"card":"fork","port":"value"}},
        {"from":{"card":"fork","port":"a"},"to":{"card":"consumer","port":"fork"}},
        {"from":{"card":"fork","port":"a"},"to":{"card":"collect","port":"in"}},
        {"from":{"card":"collect","port":"out"},"to":{"card":"consumer","port":"collect"}},
        {"from":{"card":"producer","port":"out"},"to":{"card":"merge","port":"a"}},
        {"from":{"card":"merge","port":"out"},"to":{"card":"consumer","port":"merge"}},
        {"from":{"card":"draft","port":""},"to":{"card":"consumer","port":"draft"}}
      ]
    }
  })");
  for (const auto* card : {"branch", "fork", "collect", "draft"})
    fs::write_atomic(
        root / ("definitions/" + std::string(card) + "/_card.json"),
        "{\"type\":" + json::Quote(card) + "}");
  fs::write_atomic(root / "definitions/merge/_card.json",
                   R"({"type":"merge","inputs":[{"name":"a"}]})");
  fs::write_atomic(root / "definitions/producer/_card.json",
                   R"({"type":"code"})");
  fs::write_atomic(root / "definitions/producer/_outputs.fbs",
                   "table M { value:string; } table O { out:M; } root_type O;");
  fs::write_atomic(root / "definitions/consumer/_card.json", R"({
    "type":"code","inputs":[{"name":"direct"},{"name":"branch"},{"name":"fork"},
    {"name":"collect"},{"name":"merge"},{"name":"draft"},{"name":"unwired"}]
  })");
  Store store({root, ""});
  store.open();
  const auto& graph = *store.current()->resolution;
  const auto& ports = graph.find({"consumer"})->nix_ports;
  ASSERT_EQ(ports.size(), 6u);
  for (const auto* name : {"direct", "branch", "fork", "collect"}) {
    EXPECT_EQ(ports.at(name).exports_node, graph.index.at("producer")) << name;
    EXPECT_TRUE(ports.at(name).type.resolved()) << name;
  }
  EXPECT_FALSE(ports.at("merge").exports_node);
  EXPECT_TRUE(ports.at("merge").type.resolved());
  EXPECT_FALSE(ports.at("draft").exports_node);
  EXPECT_EQ(ports.at("draft").type.type, types::empty_type());
  EXPECT_FALSE(ports.contains("unwired"));
}

TEST_F(DocumentStorage, ADraftAndARealSourceCannotShareAnInput) {
  fs::write_atomic(
      root / "tsugumi.json",
      R"({"nixPath": [],"canvas":{"cards":[{"id":"plan"},{"id":"sink"},{"id":"relay"},{"id":"condition"}],"connections":[
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

TEST_F(DocumentStorage,
       ImplementingRequiresExplicitInputAndOutputInTheSameSave) {
  const std::string project =
      R"({"nixPath": [],"canvas":{"cards":[{"id":"start"},{"id":"plan"},{"id":"next"}],"connections":[
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

TEST_F(DocumentStorage, NestedImplementationRebindsBoundaryEndsAtomically) {
  fs::write_atomic(
      root / "tsugumi.json",
      R"({"nixPath": [],"canvas":{"cards":[{"id":"start"},{"id":"outer"},{"id":"next"}],"connections":[
    {"from":{"card":"start","port":"out"},"to":{"card":"outer","port":"in"}},
    {"from":{"card":"outer","port":"out"},"to":{"card":"next","port":""}}
  ]}})");
  fs::write_atomic(root / "definitions/outer/_card.json",
                   R"({"type":"subgraph","external":"child"})");
  fs::write_atomic(root / "definitions/next/_card.json",
                   R"({"type":"draft"})");
  fs::write_atomic(
      root / "externals/child/tsugumi.json",
      R"({"canvas":{"cards":[{"id":"plan"}],"connections":[]},"boundary":{"inputs":[{"name":"in","card":"plan","port":""}],"outputs":[{"name":"out","card":"plan","port":""}]}})");
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
      R"({"canvas":{"cards":[{"id":"plan"}],"connections":[]},"boundary":{"inputs":[{"name":"in","card":"plan","port":"in"}],"outputs":[{"name":"out","card":"plan","port":"out"}]}})";
  const auto after = commit(store, {implementation, wiring});
  EXPECT_EQ(after->resolution->edges.size(), 2);
  EXPECT_EQ(after->resolution->find({"outer", "plan"})->kind,
            CardType::Collect);
}

TEST_F(DocumentStorage, NamedDraftEndIsRejectedAndIdentifiesTheWire) {
  fs::write_atomic(root / "definitions/plan/_card.json",
                   R"({"type":"draft"})");
  Store store({root, ""});
  store.open();
  ChangeIn wiring{};
  wiring.kind = ChangeIn::Kind::SetProject;
  wiring.json =
      R"({"nixPath": [],"canvas":{"cards":[{"id":"start"},{"id":"plan"}],"connections":[{"from":{"card":"start","port":"out"},"to":{"card":"plan","port":"in"}}]}})";
  try {
    commit(store, {wiring});
    FAIL();
  } catch (const Failed& error) {
    EXPECT_EQ(error.failure.kind, FailureKind::validation_failed);
  }
}
}  // namespace
}  // namespace tsugumi
