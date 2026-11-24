#include <gtest/gtest.h>

#include <set>

#include "test_document.hh"
#include "tsugumi/document/graph.hh"
#include "tsugumi/util/crypto.hh"
#include "worker_selection.hh"

namespace tsugumi {
namespace {

auto Invalid(const std::function<void()>& load, std::string_view message,
             std::string_view file = {}) -> void {
  try {
    load();
    ADD_FAILURE() << "accepted invalid document";
  } catch (const DocumentError& error) {
    EXPECT_EQ(error.code, "invalid_document");
    EXPECT_NE(std::string_view(error.what()).find(message),
              std::string_view::npos)
        << error.what();
    EXPECT_NE(std::string_view(error.what()).find(file), std::string_view::npos)
        << error.what();
  }
}

TEST(ProjectDocument, RequiresEveryProjectFieldAndChecksItsFormat) {
  const auto complete = json::Object(test::project_json());
  for (const auto& [field, _] : complete) {
    SCOPED_TRACE(field);
    auto project = complete;
    project.erase(field);
    Invalid([&] { parse_project_json(json::Encode(project)); }, field,
            "tsugumi.json");
  }
  auto project = complete;
  project["formatVersion"] = std::to_string(kProjectFormatVersion + 1);
  for (const std::string path :
       {"tsugumi.json", "externals/child/tsugumi.json"})
    Invalid([&] { parse_external_project(path, json::Encode(project)); },
            path + " has project format " +
                std::to_string(kProjectFormatVersion + 1) +
                "; this Tsugumi reads format " +
                std::to_string(kProjectFormatVersion) +
                "; there is no migration before v1.0");
}

TEST(ProjectDocument, EnforcesRequiredAndEffectiveFieldsForEveryCardType) {
  for (const std::string kind :
       {"code", "input", "merge", "collect", "draft", "trigger", "subgraph"}) {
    SCOPED_TRACE(kind);
    auto fields = json::Object(test::card_json(
        kind == "merge" ? R"({"type":"merge","inputs":[{"name":"in"}]})"
                        : "{\"type\":" + json::Quote(kind) + "}"));
    ASSERT_NO_THROW(parse_card_json("card", json::Encode(fields)));
    for (const auto& [field, _] : fields) {
      auto without = fields;
      without.erase(field);
      Invalid([&] { parse_card_json("card", json::Encode(without)); }, field,
              "definitions/card/_card.json");
    }
    const std::string forbidden =
        fields.contains("maxRuns") ? "boundary" : "maxRuns";
    fields[forbidden] = "null";
    Invalid([&] { parse_card_json("card", json::Encode(fields)); },
            forbidden + " is not allowed on type " + kind);
  }
}

TEST(ProjectDocument, RejectsInvalidValuesAndNames) {
  struct CardCase {
    const char* patch;
    const char* error;
  };
  for (
      const auto& [patch, error] : {
          CardCase{R"({"type":"merge","inputs":[]})", "inputs"},
          CardCase{R"({"type":"code","inputs":[{"name":".hidden"}]})",
                   "inputs"},
          CardCase{R"({"type":"code","inputs":[{"name":"In"}]})", "inputs"},
          CardCase{R"({"type":"code","inputs":[{"name":"in"},{"name":"in"}]})",
                   "duplicate"},
          CardCase{R"({"type":"code","maxRuns":0})", "maxRuns"},
          CardCase{R"({"type":"code","recordIO":true})", "recordIO"},
          CardCase{R"({"type":"code","timeoutSeconds":-1})", "timeoutSeconds"},
          CardCase{R"({"type":"code","systems":[]})", "systems"},
          CardCase{R"({"type":"code","systems":["riscv64-linux"]})", "systems"},
          CardCase{R"({"type":"code","systems":["x86_64-linux","x86_64-linux"]})",
                   "duplicate systems"},
          CardCase{R"({"type":"code","workerCapabilities":[64]})",
                   "workerCapabilities"},
          CardCase{R"({"type":"code","workerCapabilities":[0,0]})",
                   "workerCapabilities"},
          CardCase{R"({"type":"code","secrets":["Secret"]})", "secrets"},
          CardCase{R"({"type":"code","secrets":["secret","secret"]})",
                   "duplicate"},
          CardCase{R"({"type":"code","sandbox":{"filesystem":true}})",
                   "sandbox.pid"},
          CardCase{
              R"({"type":"code","sandbox":{"filesystem":true,"pid":true,"ipc":true,"identity":true,"limits":{"cpu":257,"memoryMiB":0}}})",
              "sandbox.limits.cpu"},
          CardCase{
              R"({"type":"code","sandbox":{"filesystem":true,"pid":true,"ipc":true,"identity":true,"limits":{"cpu":0,"memoryMiB":1048577}}})",
              "sandbox.limits.memoryMiB"},
          CardCase{R"({"type":"code","grants":{"network":false}})",
                   "grants.gui"},
          CardCase{
              R"({"type":"code","grants":{"network":false,"gui":false,"hostPaths":[{"name":"data","path":"/data"}]}})",
              "grants.hostPaths.write"},
      }) {
    SCOPED_TRACE(patch);
    Invalid([&] { parse_card_json("card", test::card_json(patch)); }, error);
  }
  for (const auto* name : {"Upper", ".hidden", "my-card"}) {
    Invalid(
        [&] { parse_card_json(name, test::card_json(R"({"type":"draft"})")); },
        name);
    Invalid(
        [&] {
          parse_project_json(test::project_json(
              "{\"canvas\":{\"connections\":[],\"cards\":[{\"id\":" +
              json::Quote(name) + "}]}}"));
        },
        name);
  }
  Invalid(
      [&] {
        parse_external_project(
            "externals/child/tsugumi.json",
            test::project_json(
                R"({"boundary":{"inputs":[],"outputs":[{"name":"Out","card":"leaf","port":"out"}]}})"));
      },
      "boundary name");
  Invalid(
      [&] {
        parse_external_project(
            "externals/child/tsugumi.json",
            test::project_json(
                R"({"boundary":{"inputs":[],"outputs":[{"name":"out","card":".leaf","port":"out"}]}})"));
      },
      "boundary card");
  Invalid(
      [&] {
        parse_project_json(test::project_json(
            R"({"canvas":{"cards":[],"connections":[{"from":{"card":"leaf","port":"Out"},"to":{"card":"other","port":"in"}}]}})"));
      },
      "connections port");
  const auto catalog = types::parse_catalog(
      "table Val {} table Outputs { Out:Val; } root_type Outputs;",
      "_outputs.fbs");
  ASSERT_EQ(catalog.errors.size(), 1u);
  EXPECT_NE(catalog.errors.front().find("output port Out"), std::string::npos);
}

class ProjectStructure : public ::testing::Test {
 protected:
  std::filesystem::path root = std::filesystem::temp_directory_path() /
                               ("p12-document-" + crypto::uuid4());

  auto TearDown() -> void override { std::filesystem::remove_all(root); }

  auto Put(std::string_view path, std::string_view contents) -> void {
    fs::write_atomic(root / path, contents);
  }

  auto Define(std::string_view prefix, std::string_view id,
              std::string_view fields) -> void {
    const auto path =
        std::string(prefix) + "definitions/" + std::string(id) + "/";
    Put(path + "_card.json", test::card_json(fields));
    Put(path + "README.md", test::readme(id));
  }
};

TEST_F(ProjectStructure, EnforcesDefinitionAndExternalOwnershipAtEveryLevel) {
  const auto load = [&] { load_from_disk(root); };
  Put("tsugumi.json",
      test::project_json(
          R"({"canvas":{"cards":[{"id":"child"}],"connections":[]}})"));
  Invalid(load, "definitions/child/");
  Define("", "child", R"({"type":"subgraph"})");
  Invalid(load, "externals/child/tsugumi.json");
  Put("externals/child/tsugumi.json",
      test::project_json(
          R"({"canvas":{"cards":[{"id":"leaf"}],"connections":[]}})"));
  Invalid(load, "externals/child/definitions/leaf/");
  Define("externals/child/", "leaf", R"({"type":"draft"})");
  ASSERT_NO_THROW(load());
  std::filesystem::remove(root / "externals/child/definitions/leaf/README.md");
  Invalid(load, "externals/child/definitions/leaf/README.md");
  Put("externals/child/definitions/leaf/README.md", "---\ndescription:\n---\n");
  Invalid(load, "front matter needs title");
  Put("externals/child/definitions/leaf/README.md", test::readme("Leaf"));
  std::filesystem::create_directories(root /
                                      "externals/child/definitions/extra");
  Invalid(load, "externals/child/definitions/extra/ has no canvas Card");
  std::filesystem::remove(root / "externals/child/definitions/extra");
  std::filesystem::create_directories(root /
                                      "externals/child/externals/orphan");
  Invalid(load, "externals/child/externals/orphan/ has no subgraph Card");
  std::filesystem::remove(root / "externals/child/externals/orphan");
  std::filesystem::create_directories(root / "externals/.hidden");
  Invalid(load, ".hidden");
  std::filesystem::remove(root / "externals/.hidden");
  std::filesystem::remove(root / "definitions/child/README.md");
  Invalid(load, "definitions/child/README.md");
  Put("definitions/child/README.md", test::readme(""));
  Invalid(load, "title must be non-empty");
}

TEST_F(ProjectStructure, ResolvesRecordSettingsAgainstTheCardProject) {
  Put("tsugumi.json",
      test::project_json(
          R"({"recordLogs":false,"recordIO":true,"canvas":{"cards":[{"id":"inherit"},{"id":"override"}],"connections":[]}})"));
  Define("", "inherit", R"({"type":"code","workerCapabilities":[0,63]})");
  Define(
      "", "override",
      R"({"type":"code","recordLogs":"on","recordIO":"off","timeoutSeconds":13,"buildTimeoutSeconds":17})");
  const auto graph = resolve(*load_from_disk(root), [](const std::string&) {
    return std::optional<std::string>{};
  });
  const auto& a = graph->find({"inherit"})->settings;
  EXPECT_FALSE(a.record_logs);
  EXPECT_TRUE(a.record_io);
  EXPECT_FALSE(a.timeout_seconds);
  EXPECT_FALSE(a.build_timeout_seconds);
  EXPECT_FALSE(a.sandbox.limits.limited());
  EXPECT_EQ(a.worker_capabilities, (uint64_t{1} << 63) | 1);
  const auto& b = graph->find({"override"})->settings;
  EXPECT_TRUE(b.record_logs);
  EXPECT_FALSE(b.record_io);
  EXPECT_EQ(b.timeout_seconds, 13);
  EXPECT_EQ(b.build_timeout_seconds, 17);
}

auto References(const Resolution& graph, const CardPath& card)
    -> std::set<std::pair<std::string, std::string>> {
  std::set<std::pair<std::string, std::string>> references;
  for (const auto& ref : graph.find(card)->nix_references)
    references.emplace(ref.name, path_key(ref.target));
  return references;
}

using Refs = std::set<std::pair<std::string, std::string>>;

TEST_F(ProjectStructure, SubgraphExportsGoUnderTheSubgraphCardId) {
  Put("tsugumi.json",
      test::project_json(
          R"({"canvas":{"cards":[{"id":"start"},{"id":"sub"},{"id":"sink"}],"connections":[{"from":{"card":"start","port":"out"},"to":{"card":"sub","port":"in"}},{"from":{"card":"sub","port":"out"},"to":{"card":"sink","port":"in"}},{"from":{"card":"sub","port":"out2"},"to":{"card":"sink","port":"in2"}}]}})"));
  Define("", "start", R"({"type":"trigger"})");
  Define("", "sub", R"({"type":"subgraph"})");
  Define("", "sink",
         R"({"type":"code","inputs":[{"name":"in"},{"name":"in2"}]})");
  Put("externals/sub/tsugumi.json",
      test::project_json(
          R"({"canvas":{"cards":[{"id":"leaf"},{"id":"leaf2"}],"connections":[]},"boundary":{"inputs":[{"name":"in","card":"leaf","port":"in"}],"outputs":[{"name":"out","card":"leaf","port":"out"},{"name":"out2","card":"leaf2","port":"out"}]}})"));
  for (const auto* id : {"leaf", "leaf2"}) {
    Define("externals/sub/", id,
           R"({"type":"code","inputs":[{"name":"in"}]})");
    Put(std::string("externals/sub/definitions/") + id + "/_outputs.fbs",
        "table Value {} table Outputs { out:Value; } root_type Outputs;");
  }
  const auto graph = resolve(
      *load_from_disk(root),
      [&](const std::string& path) { return fs::read_file(root / path); });
  // Inner ids never appear outside, and inner Cards get nothing from outside.
  EXPECT_EQ(References(*graph, {"sink"}),
            (Refs{{"sub", "sub/leaf"}, {"sub", "sub/leaf2"}}));
  EXPECT_TRUE(graph->find({"sub", "leaf"})->nix_references.empty());
}

TEST_F(ProjectStructure, MergeAndCollectPassOnExports) {
  Put("tsugumi.json",
      test::project_json(
          R"({"canvas":{"cards":[{"id":"shared"},{"id":"a"},{"id":"b"},{"id":"m"},{"id":"c"},{"id":"sink"}],"connections":[{"from":{"card":"shared","port":"out"},"to":{"card":"a","port":"in"}},{"from":{"card":"shared","port":"out"},"to":{"card":"b","port":"in"}},{"from":{"card":"a","port":"out"},"to":{"card":"m","port":"a"}},{"from":{"card":"b","port":"out"},"to":{"card":"m","port":"b"}},{"from":{"card":"a","port":"out"},"to":{"card":"c","port":"in"}},{"from":{"card":"m","port":"out"},"to":{"card":"sink","port":"m"}},{"from":{"card":"c","port":"out"},"to":{"card":"sink","port":"c"}}]}})"));
  for (const auto* id : {"shared", "a", "b"}) {
    Define("", id, R"({"type":"code","inputs":[{"name":"in"}]})");
    Put(std::string("definitions/") + id + "/_outputs.fbs",
        "table Value {} table Outputs { out:Value; } root_type Outputs;");
  }
  Define("", "m", R"({"type":"merge","inputs":[{"name":"a"},{"name":"b"}]})");
  Define("", "c", R"({"type":"collect"})");
  Define("", "sink",
         R"({"type":"code","inputs":[{"name":"m"},{"name":"c"}]})");
  const auto graph = resolve(
      *load_from_disk(root),
      [&](const std::string& path) { return fs::read_file(root / path); });
  // Merge passes on only what a and b share; Collect passes on what a shows.
  EXPECT_EQ(References(*graph, {"sink"}),
            (Refs{{"a", "a"}, {"shared", "shared"}}));
}

TEST_F(ProjectStructure, DraftWireEndsHaveNoPort) {
  const auto project = [&](std::string_view draft_end,
                           std::string_view code_end) {
    Put("tsugumi.json",
        test::project_json(
            R"({"canvas":{"cards":[{"id":"idea"},{"id":"work"}],"connections":[{"from":)" +
            std::string(draft_end) + R"(,"to":)" + std::string(code_end) +
            "}]}}"));
  };
  Define("", "idea", R"({"type":"draft"})");
  Define("", "work", R"({"type":"code","inputs":[{"name":"in"}]})");
  const auto load = [&] { load_from_disk(root); };
  project(R"({"card":"idea"})", R"({"card":"work","port":"in"})");
  ASSERT_NO_THROW(load());
  project(R"({"card":"idea","port":"out"})", R"({"card":"work","port":"in"})");
  Invalid(load, "must not name a port");
  project(R"({"card":"idea","port":""})", R"({"card":"work","port":"in"})");
  Invalid(load, "connections port");
  project(R"({"card":"idea"})", R"({"card":"work"})");
  Invalid(load, "needs a port");
}

TEST(RunSystem, NativeWhenListedOtherwiseTheFirstEntry) {
  Settings settings;
  settings.systems = {"aarch64-linux", "x86_64-linux"};
  EXPECT_EQ(run_system(settings, "x86_64-linux"), "x86_64-linux");
  EXPECT_EQ(run_system(settings, "aarch64-linux"), "aarch64-linux");
  EXPECT_EQ(run_system(settings, "aarch64-darwin"), "aarch64-linux");
}

TEST(WorkerSelection, MatchesEveryRequiredBitAndPrefersTheFewestBits) {
  constexpr uint64_t needed = uint64_t{1} << 63;
  constexpr WorkerCandidate workers[] = {{"busy", needed, false},
                                         {"local", 1, true},
                                         {"broad", needed | 7, true},
                                         {"first", needed | 8, true},
                                         {"second", needed | 16, true}};
  EXPECT_EQ(select_worker(workers, needed).index, 3);
  EXPECT_EQ(select_worker(workers, needed, "broad").index, 2);
  const auto resident = select_worker(workers, needed, "busy");
  EXPECT_TRUE(resident.matches);
  EXPECT_FALSE(resident.index);
  EXPECT_FALSE(select_worker(workers, needed | 32).matches);
}

}  // namespace
}  // namespace tsugumi
