#include <gtest/gtest.h>

#include "nix_params.hh"
#include "scheduler.hh"
#include "store.hh"
#include "test_document.hh"
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
        test::project_json(
            R"({"nixPath": [],"canvas":{"cards":[{"id":"start"}],"connections":[]}})"));
    test::write_card(root / "definitions/start/_card.json",
                     test::card_json(R"({"type":"trigger"})"));
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
  const std::string readme =
      "---\ntitle: Start\ndescription: Start the run\n---\n";
  fs::write_atomic(root / "definitions/start/README.md", readme);
  Store store({root, ""});
  store.open();
  const auto before = store.current();
  ASSERT_EQ(before->definitions.at("start").readme, readme);
  ChangeIn put{};
  put.kind = ChangeIn::Kind::PutDefinition;
  put.id = "start";
  put.json = test::card_json(R"({"type":"trigger"})");
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
  const auto parsed = parse_project_json(test::project_json(
      R"({"name":"[ ] { } [\" ]","canvas":{"connections":[],"cards":[]},"nixPath": []})"));
  EXPECT_EQ(parse_project_json(parsed.text).text, parsed.text);
  EXPECT_EQ(document_text::root<Tsugumi::Document::Project>(parsed.buffer).name()->str(),
            R"([ ] { } [" ])");
}

TEST_F(DocumentStorage, ResolvedNixPathUsesTheCardsProject) {
  fs::write_atomic(
      root / "tsugumi.json",
      test::project_json(
          R"({"canvas":{"cards":[{"id":"start"}],"connections":[]},"nixPath":[{"name":"science","url":"github:org/science"}]})"));
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
      exp = 1.0e3;
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
                                    "{ max = 9223372036854775807; }"),
            R"([{"name":"max","type":"int","value":9223372036854775807}])");
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

TEST_F(DocumentStorage, CheckedParamsPreserveLiteralTypes) {
  fs::write_atomic(
      root / "tsugumi.json",
      test::project_json(
          R"({"canvas":{"cards":[{"id":"start"},{"id":"a"}],"connections":[]}})"));
  test::write_card(root / "definitions/a/_card.json",
                   test::card_json(R"({"type":"code"})"));
  fs::write_atomic(root / "definitions/a/params.nix",
                   "{ z = 0.0; a = false; count = -5; preset = 5.0; }");
  Store store({root, ""});
  store.open();
  const auto* node = store.current()->resolution->find({"a"});
  ASSERT_NE(node, nullptr);
  EXPECT_TRUE(node->diags.empty());
  const auto& params = node->settings.params;
  ASSERT_EQ(params.size(), 4u);
  EXPECT_EQ(params[0].data, "false");
  EXPECT_EQ(params[1].data, "-5");
  EXPECT_EQ(params[2].type, ParamType::Double);
  EXPECT_EQ(params[2].data, "5.0");
  EXPECT_EQ(params[3].data, "0.0");
}

TEST(ParamsNix, RejectsOversizedSourceAndValues) {
  const std::string source(kMaxDocumentJsonBytes + 1, ' ');
  try {
    evaluate_literal_params("definitions/work/", source);
    FAIL() << "oversized parameter source was accepted";
  } catch (const std::runtime_error& error) {
    EXPECT_STREQ(error.what(),
                 "definitions/work/params.nix: exceeds the 8 MiB document limit");
  }

  // The Nix source can contain literal tabs, which JSON encodes as two bytes.
  const auto values = "{ value = \"" +
                      std::string(kMaxDocumentJsonBytes / 2, '\t') + "\"; }";
  try {
    evaluate_literal_params("definitions/work/", values);
    FAIL() << "oversized parameter JSON was accepted";
  } catch (const std::runtime_error& error) {
    EXPECT_STREQ(error.what(),
                 "definitions/work/params.nix: params.nix values exceed 8 MiB");
  }
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
      test::project_json(
          R"({"nixPath": [],"canvas":{"cards":[{"id":"start"},{"id":"b"}],"connections":[{"from":{"card":"start","port":"out"},"to":{"card":"b","port":"config"}}]}})"));
  test::write_card(root / "definitions/b/_card.json",
                   test::card_json(R"({"type":"code"})"));
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
       AnEndWithoutAPortOnAnImplementedCardIsALoadError) {
  fs::write_atomic(
      root / "tsugumi.json",
      test::project_json(
          R"({"nixPath": [],"canvas":{"cards":[{"id":"start"},{"id":"b"}],"connections":[{"from":{"card":"start","port":"out"},"to":{"card":"b"}}]}})"));
  test::write_card(root / "definitions/b/_card.json",
                   test::card_json(R"({"type":"code"})"));
  Store store({root, ""});
  try {
    store.open();
    FAIL() << "a port-less end on a Code Card must not load";
  } catch (const std::exception& error) {
    EXPECT_NE(std::string_view(error.what()).find("needs a port"),
              std::string_view::npos)
        << error.what();
  }
}
TEST_F(DocumentStorage, ProjectNameDoesNotChangeExecution) {
  Store store({root, ""});
  store.open();
  auto before = store.current();
  ChangeIn project{};
  project.kind = ChangeIn::Kind::SetProject;
  project.json = test::project_json(
      R"({"name":"Renamed","nixPath": [],"canvas":{"cards":[{"id":"start"}],"connections":[]}})");
  auto after = commit(store, {project});
  EXPECT_EQ(after->execution_rev, before->execution_rev);
  EXPECT_EQ(after->execution_fingerprint, before->execution_fingerprint);
  EXPECT_EQ(after->project_json, parse_project_json(project.json).text);
  ChangeIn card{};
  card.kind = ChangeIn::Kind::PutDefinition;
  card.id = "start";
  card.keep_readme = true;
  card.json = test::card_json(R"({"type":"draft"})");
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
  project.json = test::project_json(
      R"({ "canvas": { "connections": [ ], "cards": [ { "id": "start" } ] }, "name": "Renamed", "nixPath": [] })");
  ChangeIn card{};
  card.kind = ChangeIn::Kind::PutDefinition;
  card.id = "start";
  card.keep_readme = true;
  card.json = test::card_json(R"({ "type": "trigger" })");
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
      test::project_json(
          R"({"nixPath": [],"canvas":{"cards":[{"id":"outer"}],"connections":[]}})"));
  std::filesystem::remove_all(root / "definitions/start");
  test::write_card(root / "definitions/outer/_card.json",
                   test::card_json(R"({"type":"subgraph"})"));
  const std::string project = test::project_json(
      R"({"canvas":{"cards":[{"id":"start"}],"connections":[]},"boundary":{"inputs":[],"outputs":[]}})");
  fs::write_atomic(root / "externals/outer/tsugumi.json", project);
  test::write_card(root / "externals/outer/definitions/start/_card.json",
                   test::card_json(R"({"type":"trigger"})"));
  fs::write_atomic(root / "externals/outer/definitions/start/README.md",
                   test::readme("Start", "Original description"));
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
      {put("externals/outer/tsugumi.json",
           test::project_json(
               R"({"name":"Renamed","canvas":{"cards":[{"id":"start"}],"connections":[]},"boundary":{"inputs":[],"outputs":[]}})")),
       put("externals/outer/definitions/start/README.md",
           test::readme("Start", "Updated description"))});
  EXPECT_EQ(after->execution_fingerprint, before->execution_fingerprint);
  EXPECT_EQ(after->execution_rev, before->execution_rev);
  ASSERT_NE(after->resolution->find({"outer", "start"}), nullptr);
  EXPECT_TRUE(after->resolution->find({"outer", "start"})->diags.empty());
  after = commit(store, {put("externals/outer/definitions/start/_card.json",
                             test::card_json(R"({"type":"draft"})"))});
  EXPECT_NE(after->execution_fingerprint, before->execution_fingerprint);
}
TEST_F(DocumentStorage, DraftHasNoPortsFromMetadataOrSchema) {
  test::write_card(root / "definitions/start/_card.json",
                   test::card_json(R"({"type":"draft"})"));
  fs::write_atomic(root / "definitions/start/README.md",
                   test::readme("Start", "A future port called planned."));
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
  put.json = test::card_json(R"({"type":"draft"})");
  put.readme = test::readme("Start", "A future port called different.");
  auto after = commit(store, {put});
  EXPECT_EQ(after->resolution, before->resolution);
}

TEST_F(DocumentStorage, ASecondWireSuggestsMerge) {
  fs::write_atomic(
      root / "tsugumi.json",
      test::project_json(
          R"({"nixPath":[],"canvas":{"cards":[{"id":"start"},{"id":"plan"},{"id":"sink"}],"connections":[
    {"from":{"card":"plan"},"to":{"card":"sink","port":"in"}},
    {"from":{"card":"start","port":"out"},"to":{"card":"sink","port":"in"}}
  ]}})"));
  test::write_card(root / "definitions/plan/_card.json",
                   test::card_json(R"({"type":"draft"})"));
  test::write_card(
      root / "definitions/sink/_card.json",
      test::card_json(R"({"type":"merge","inputs":[{"name":"in"}]})"));
  Store store({root, ""});
  store.open();
  const auto& graph = *store.current()->resolution;
  ASSERT_EQ(graph.edges.size(), 1u);
  const auto& diagnostics = graph.find({"sink"})->diags;
  ASSERT_EQ(diagnostics.size(), 1u);
  EXPECT_EQ(diagnostics.front().code, "input_already_connected");
  EXPECT_NE(diagnostics.front().message.find("Merge"), std::string::npos);
}

TEST_F(DocumentStorage,
       ImplementingRequiresExplicitInputAndOutputInTheSameSave) {
  const std::string project = test::project_json(
      R"({"nixPath": [],"canvas":{"cards":[{"id":"start"},{"id":"plan"},{"id":"next"}],"connections":[
    {"from":{"card":"start","port":"out"},"to":{"card":"plan"}},
    {"from":{"card":"plan"},"to":{"card":"next"}}
  ]}})");
  fs::write_atomic(root / "tsugumi.json", project);
  test::write_card(root / "definitions/plan/_card.json",
                   test::card_json(R"({"type":"draft"})"));
  test::write_card(root / "definitions/next/_card.json",
                   test::card_json(R"({"type":"draft"})"));
  Store store({root, ""});
  store.open();
  auto before = store.current();
  ChangeIn implementation{};
  implementation.kind = ChangeIn::Kind::PutDefinition;
  implementation.id = "plan";
  implementation.keep_readme = true;
  implementation.json = test::card_json(R"({"type":"collect"})");
  try {
    commit(store, {implementation});
    FAIL() << "Even the only input must be stated";
  } catch (const Failed& error) {
    EXPECT_EQ(error.failure.kind, FailureKind::invalid_document);
  }
  EXPECT_EQ(store.current(), before);
  EXPECT_EQ(fs::read_file(root / "definitions/plan/_card.json"),
            test::card_json(R"({"type":"draft"})"));
  ChangeIn wiring{};
  wiring.kind = ChangeIn::Kind::SetProject;
  wiring.json = project;
  const auto input = wiring.json.find(R"("card":"plan"})");
  wiring.json.replace(input, std::string(R"("card":"plan"})").size(),
                      R"("card":"plan","port":"in"})");
  try {
    commit(store, {implementation, wiring});
    FAIL() << "The output must also be stated";
  } catch (const Failed& error) {
    EXPECT_EQ(error.failure.kind, FailureKind::invalid_document);
  }
  const auto output = wiring.json.find(R"("card":"plan"})");
  wiring.json.replace(output, std::string(R"("card":"plan"})").size(),
                      R"("card":"plan","port":"out"})");
  auto after = commit(store, {implementation, wiring});
  EXPECT_EQ(after->resolution->edges.size(), 2);
  EXPECT_EQ(after->resolution->find({"plan"})->kind, CardType::Collect);
  EXPECT_TRUE(after->resolution->find({"next"})->inputs.empty());
}

TEST_F(DocumentStorage, NestedImplementationRebindsBoundaryEndsAtomically) {
  fs::write_atomic(
      root / "tsugumi.json",
      test::project_json(
          R"({"nixPath": [],"canvas":{"cards":[{"id":"start"},{"id":"outer"},{"id":"next"}],"connections":[
    {"from":{"card":"start","port":"out"},"to":{"card":"outer","port":"in"}},
    {"from":{"card":"outer","port":"out"},"to":{"card":"next"}}
  ]}})"));
  test::write_card(root / "definitions/outer/_card.json",
                   test::card_json(R"({"type":"subgraph"})"));
  test::write_card(root / "definitions/next/_card.json",
                   test::card_json(R"({"type":"draft"})"));
  fs::write_atomic(
      root / "externals/outer/tsugumi.json",
      test::project_json(
          R"({"canvas":{"cards":[{"id":"plan"}],"connections":[]},"boundary":{"inputs":[{"name":"in","card":"plan"}],"outputs":[{"name":"out","card":"plan"}]}})"));
  test::write_card(root / "externals/outer/definitions/plan/_card.json",
                   test::card_json(R"({"type":"draft"})"));
  Store store({root, ""});
  store.open();
  ASSERT_EQ(store.current()->resolution->edges.size(), 2);
  ChangeIn implementation{};
  implementation.kind = ChangeIn::Kind::PutSource;
  implementation.path = "externals/outer/definitions/plan/_card.json";
  implementation.base_hash =
      store.current()->sources.at(implementation.path).hash;
  implementation.bytes = test::card_json(R"({"type":"collect"})");
  try {
    commit(store, {implementation});
    FAIL();
  } catch (const Failed& error) {
    EXPECT_EQ(error.failure.kind, FailureKind::invalid_document);
  }
  EXPECT_EQ(store.current()->resolution->find({"outer", "plan"})->kind,
            CardType::Draft);
  ChangeIn wiring{};
  wiring.kind = ChangeIn::Kind::PutSource;
  wiring.path = "externals/outer/tsugumi.json";
  wiring.base_hash = store.current()->sources.at(wiring.path).hash;
  wiring.bytes = test::project_json(
      R"({"canvas":{"cards":[{"id":"plan"}],"connections":[]},"boundary":{"inputs":[{"name":"in","card":"plan","port":"in"}],"outputs":[{"name":"out","card":"plan","port":"out"}]}})");
  const auto after = commit(store, {implementation, wiring});
  EXPECT_EQ(after->resolution->edges.size(), 2);
  EXPECT_EQ(after->resolution->find({"outer", "plan"})->kind,
            CardType::Collect);
}

TEST_F(DocumentStorage, NamedDraftEndIsRejectedAndIdentifiesTheWire) {
  test::write_card(root / "definitions/plan/_card.json",
                   test::card_json(R"({"type":"draft"})"));
  fs::write_atomic(root / "tsugumi.json", test::project_json(
      R"({"canvas":{"cards":[{"id":"start"},{"id":"plan"}],"connections":[]}})"));
  Store store({root, ""});
  store.open();
  ChangeIn wiring{};
  wiring.kind = ChangeIn::Kind::SetProject;
  wiring.json = test::project_json(
      R"({"nixPath": [],"canvas":{"cards":[{"id":"start"},{"id":"plan"}],"connections":[{"from":{"card":"start","port":"out"},"to":{"card":"plan","port":"in"}}]}})");
  try {
    commit(store, {wiring});
    FAIL();
  } catch (const Failed& error) {
    EXPECT_EQ(error.failure.kind, FailureKind::invalid_document);
  }
}
}  // namespace
}  // namespace tsugumi
