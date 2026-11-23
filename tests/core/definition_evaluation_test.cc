#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <future>

#include "nix_eval.hh"
#include "tsugumi/document/document.hh"
#include "tsugumi/document/graph.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/util/json_object.hh"

namespace tsugumi {
namespace {

TEST(DefinitionEvaluationKey, SeparatesDefinitionContentAndTargetSystem) {
  NixCall original;
  original.definition_key = "resolved-lock-wiring-params-and-sources";
  original.system = "aarch64-darwin";
  const auto key = original.evaluation_key();
  auto changed = original;
  changed.system = "aarch64-linux";
  EXPECT_NE(key, changed.evaluation_key());
  changed = original;
  changed.definition_key = "edited";
  EXPECT_NE(key, changed.evaluation_key());

  auto operational = original;
  operational.root_prefix = "different-card";
  operational.root_dir = "/different-project";
  operational.deadline = std::chrono::steady_clock::now();
  EXPECT_EQ(key, operational.evaluation_key());
}

class DefinitionEvaluation : public ::testing::Test {
 protected:
  static auto SetUpTestSuite() -> void { NixEvaluator::init_process(); }
  auto SetUp() -> void override {
    root = std::filesystem::canonical(std::filesystem::temp_directory_path()) /
           ("evaluation-" + crypto::uuid4());
    std::filesystem::create_directories(root / "source");
    fs::write_atomic(root / "source" / "value", "first");
    const char* lock = std::getenv("TSUGUMI_TEST_NIX_LOCK");
    ASSERT_NE(lock, nullptr) << "run using nix develop or tsugumi-test-env";
    for (const auto& value : json::Array(json::Object(lock).at("entries"))) {
      const auto row = json::Object(value);
      const NixSource source{json::Text(row, "url"), json::Text(row, "rev"),
                             json::Text(row, "narHash")};
      const auto name = json::Text(row, "name");
      pins.emplace(name, source);
      if (name == "nixpkgs") call.core_nixpkgs = source;
    }
    call.system = native_system();
    call.card = "work";
    call.definition_dir = root / "source";
    call.definitions = {{call.definition_dir.string(), call.card, ""}};
    call.projects.emplace("", LockedNixPath{});
    call.args =
        "{ root = /. + " + json::Quote(call.definition_dir.string()) + "; }";
    call.root_dir = root / "roots";
    call.deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(120);
  }
  auto TearDown() -> void override { std::filesystem::remove_all(root); }
  auto evaluator(const std::string& body) -> std::unique_ptr<NixEvaluator> {
    return std::make_unique<NixEvaluator>(
        "args: { executionList = false; implementationCount "
        "= 1; "
        "implementationLabels = builtins.toJSON [ \"\" ]; "
        "plan = (" +
        body + "); }");
  }
  std::filesystem::path root;
  NixCall call;
  LockedNixPath pins;
};

TEST_F(DefinitionEvaluation, CancellationRemovesAnEvaluationBehindABusyOne) {
  auto eval = evaluator("builtins.readFile (args.root + \"/value\")");
  std::promise<void> entered, enqueued, release;
  auto gate = release.get_future();
  auto busy_call = call;
  busy_call.on_phase = [&](bool) {
    entered.set_value();
    gate.wait();
  };
  auto busy =
      std::async(std::launch::async, [&] { eval->evaluate(busy_call); });
  if (entered.get_future().wait_for(std::chrono::seconds(5)) !=
      std::future_status::ready) {
    release.set_value();
    busy.get();
    FAIL() << "first evaluation did not enter";
  }

  std::stop_source stop;
  auto queued = call;
  queued.cancel = stop.get_token();
  queued.on_phase = [&](bool) { enqueued.set_value(); };
  auto waiting = std::async(std::launch::async, [&] {
    EXPECT_THROW(eval->evaluate(queued), NixInterrupted);
  });
  const auto admission =
      enqueued.get_future().wait_for(std::chrono::seconds(5));
  stop.request_stop();
  // Bound the deadlock assertion so a regression can still release the gate.
  const auto outcome = waiting.wait_for(std::chrono::seconds(5));
  release.set_value();
  waiting.get();
  busy.get();
  EXPECT_EQ(admission, std::future_status::ready);
  EXPECT_EQ(outcome, std::future_status::ready);
}

TEST_F(DefinitionEvaluation,
       UnchangedReadsHitAndSourceContentsInvalidateCachedImports) {
  fs::write_atomic(root / "source" / "default.nix",
                   "builtins.readFile ./value");
  auto eval = evaluator("import (args.root + \"/default.nix\")");
  auto first = eval->realise_native(call, eval->evaluate(call));
  ASSERT_NE(first.evaluation_epoch, 0u);
  EXPECT_TRUE(eval->reuse(call, first));
  fs::write_atomic(root / "source" / "value", "second");
  EXPECT_FALSE(eval->reuse(call, first));
  auto second = eval->evaluate(call);
  EXPECT_NE(first.derivation.path, second.derivation.path);
}

TEST_F(DefinitionEvaluation, DirectoryMembershipAndNegativeLookupsAreInputs) {
  auto eval = evaluator(
      "if builtins.pathExists (args.root + \"/optional\") "
      "then builtins.readFile (args.root + \"/optional\") "
      "else builtins.toJSON (builtins.readDir args.root)");
  auto first = eval->realise_native(call, eval->evaluate(call));
  ASSERT_NE(first.evaluation_epoch, 0u);
  EXPECT_TRUE(eval->reuse(call, first));
  fs::write_atomic(root / "source" / "optional", "added");
  EXPECT_FALSE(eval->reuse(call, first));
  auto added = eval->realise_native(call, eval->evaluate(call));
  EXPECT_NE(first.derivation.path, added.derivation.path);
  std::filesystem::remove(root / "source" / "optional");
  EXPECT_FALSE(eval->reuse(call, added));
  EXPECT_EQ(first.derivation.path, eval->evaluate(call).derivation.path);
}

TEST_F(DefinitionEvaluation, CopiedSourceTreeInvalidatesSourceToStoreCache) {
  fs::write_atomic(root / "source" / "nested" / "page.txt", "first");
  // Interpolation copies the tree into the store, as a builder's derivation
  // inputs do. builtins.toString alone would only pass a local path string.
  auto eval = evaluator(R"("${args.root}")");
  auto first = eval->realise_native(call, eval->evaluate(call));
  ASSERT_NE(first.evaluation_epoch, 0u);
  EXPECT_TRUE(eval->reuse(call, first));
  fs::write_atomic(root / "source" / "nested" / "page.txt", "second");
  EXPECT_FALSE(eval->reuse(call, first));
  auto second = eval->evaluate(call);
  EXPECT_NE(first.derivation.path, second.derivation.path);
}

TEST_F(DefinitionEvaluation,
       TargetSystemChangesTheDerivationWithoutBuildingOutputs) {
  auto eval = evaluator("builtins.currentSystem");
  call.system = "aarch64-linux";
  const auto arm = eval->evaluate(call);
  call.system = "x86_64-linux";
  const auto intel = eval->evaluate(call);
  EXPECT_EQ(arm.derivation.system, "aarch64-linux");
  EXPECT_EQ(intel.derivation.system, "x86_64-linux");
  EXPECT_NE(arm.derivation.path, intel.derivation.path);
  EXPECT_FALSE(std::filesystem::exists(arm.derivation.output));
  EXPECT_FALSE(std::filesystem::exists(intel.derivation.output));
  call.system = "aarch64-linux";
  EXPECT_EQ(arm.derivation.path, eval->evaluate(call).derivation.path);
}

TEST_F(DefinitionEvaluation, MissingDerivationIsAMiss) {
  auto eval = evaluator(R"(builtins.readFile (args.root + "/value"))");
  auto result = eval->realise_native(call, eval->evaluate(call));
  ASSERT_NE(result.evaluation_epoch, 0u);
  result.derivation.path =
      "/nix/store/00000000000000000000000000000000-unavailable.drv";
  EXPECT_FALSE(eval->reuse(call, result));
}

TEST_F(DefinitionEvaluation, MissingOutputIsAMiss) {
  auto eval = evaluator("builtins.readFile (args.root + \"/value\")");
  auto result = eval->evaluate(call);
  // No build of this unique output: evaluation alone cannot claim availability.
  result.derivation.output =
      "/nix/store/00000000000000000000000000000000-unavailable";
  EXPECT_FALSE(eval->reuse(call, result));
}

TEST_F(DefinitionEvaluation,
       RestrictedEnvironmentAndTimeStayFixedAcrossImportRefresh) {
  auto eval = evaluator(
      "builtins.toString builtins.currentTime + builtins.getEnv "
      "\"PATH\" + builtins.readFile (args.root + \"/value\")");
  auto first = eval->realise_native(call, eval->evaluate(call));
  const auto body = fs::read_file(first.derivation.output).value();
  ASSERT_TRUE(body.ends_with("first"));
  EXPECT_EQ(body.size(), body.find("first") + 5);
  EXPECT_TRUE(std::ranges::all_of(body.substr(0, body.size() - 5),
                                  [](char c) { return c >= '0' && c <= '9'; }));
  fs::write_atomic(root / "source" / "value", "second");
  EXPECT_FALSE(eval->reuse(call, first));
  auto second = eval->realise_native(call, eval->evaluate(call));
  EXPECT_EQ(fs::read_file(second.derivation.output).value(),
            body.substr(0, body.size() - 5) + "second");
}

TEST_F(DefinitionEvaluation, SearchPathIsCatchableAndHostPathIsEmpty) {
  fs::write_atomic(root / "source" / "default.nix", R"(
    if builtins.nixPath == [] && !(builtins.tryEval <missing>).success
    then builtins.baseNameOf <local/flake.nix>
    else throw "external search path was visible"
  )");
  call.projects.at("").emplace("local", pins.at("nixpkgs"));
  auto eval = evaluator("import (args.root + \"/default.nix\")");
  auto result = eval->realise_native(call, eval->evaluate(call));
  EXPECT_EQ(fs::read_file(result.derivation.output), "flake.nix");
  fs::write_atomic(root / "source" / "default.nix", "toString <misspelled>");
  try {
    eval->evaluate(call);
    FAIL() << "undeclared name was accepted";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(
        std::string(error.what())
            .find("undeclared nixPath name 'misspelled' (declared: local)"),
        std::string::npos);
  }
}

TEST_F(DefinitionEvaluation, NameResolutionFollowsImportedDefinitionOrigin) {
  const auto external = root / "external";
  const std::string imported = R"(builtins.hashFile "sha256" <dep/flake.nix>)";
  for (const auto& dir : {root / "source", external}) {
    std::filesystem::create_directories(dir / "inner");
    fs::write_atomic(dir / "inner" / "lookup.nix", imported);
  }
  call.definitions.push_back(
      {external.string(), "outer/work", "externals/outer/"});
  call.projects.at("").emplace("dep", pins.at("nixpkgs"));
  call.projects["externals/outer/"].emplace("dep", pins.at("tsugumi-kit"));
  call.args = "{ root = /. + " + json::Quote((root / "source").string()) +
              "; ext = /. + " + json::Quote(external.string()) + "; }";
  auto eval = evaluator(R"(
    let a = import (args.root + "/inner/lookup.nix");
        b = import (args.ext + "/inner/lookup.nix");
    in if a != b then "different projects" else throw "shared search path"
  )");
  auto first = eval->realise_native(call, eval->evaluate(call));
  EXPECT_EQ(fs::read_file(first.derivation.output), "different projects");

  call.projects["externals/outer/"]["dep"] = pins.at("nixpkgs");
  EXPECT_THROW(eval->evaluate(call), std::runtime_error);
}

TEST_F(DefinitionEvaluation, FetchPrimopsAndImportFromDerivationAreDenied) {
  auto fetch = evaluator(R"(
    if (builtins.tryEval (builtins.fetchTree (throw "fetch argument forced"))).success
    then throw "fetchTree ran"
    else builtins.fetchTarball { url = "https://example.invalid/source"; sha256 = "wrong"; }
  )");
  try {
    fetch->evaluate(call);
    FAIL() << "fetchTarball was accepted";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what())
                  .find("fetchTarball is forbidden during Card evaluation"),
              std::string::npos);
  }
  auto ifd = evaluator(R"(
    builtins.readFile (builtins.derivation {
      name = "tsugumi-ifd-test";
      system = builtins.currentSystem;
      builder = "/builder-must-not-run";
    })
  )");
  try {
    ifd->evaluate(call);
    FAIL() << "import from derivation was accepted";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("allow-import-from-derivation"),
              std::string::npos);
  }
}

TEST_F(DefinitionEvaluation, ExpressionUsesTheCardDirectoryAndProject) {
  const auto dir = call.definition_dir;
  fs::write_atomic(dir / "default.nix",
                   "{ params, inputs }: { api = params.value; }");
  fs::write_atomic(dir / "relative", "from the Card");
  call.projects.at("").emplace("dep", pins.at("nixpkgs"));
  NixEvaluator eval("args: { params.value = 42; inputs = {}; }");
  const auto result = eval.expression(
      call,
      R"({ inherit inputs; x = (import ./. { inherit params inputs; }).api;
                 local = builtins.readFile ./relative;
                 lock = builtins.baseNameOf <dep/flake.nix>;
                 store = import (builtins.toFile "scope-test.nix"
                   "(builtins.tryEval <dep>).success"); })",
      false);
  EXPECT_EQ(
      result.json,
      R"({"inputs":{},"local":"from the Card","lock":"flake.nix","store":false,"x":42})");
  EXPECT_THROW(eval.expression(call,
                               "builtins.concatStringsSep \"\" "
                               "(builtins.genList (_: \"abcdefgh\") 1100000)",
                               false),
               std::runtime_error);
}

TEST_F(DefinitionEvaluation, ExpressionBuildReturnsFilesAndFailureLog) {
  call.projects.at("").emplace("dep", pins.at("nixpkgs"));
  NixEvaluator eval("args: { params = {}; inputs = {}; }");
  fs::write_atomic(call.definition_dir / "default.nix", R"(
    { params, inputs }:
    let pkgs = import <dep> {};
    in {
      run = throw "a tool must not force the Card's run";
      api = pkgs.runCommand "expression-api-test" {} ''
        mkdir -p "$out/share/tsugumi/api"
        printf '\000\377' > "$out/share/tsugumi/api/result"
        ln -s /etc/hosts "$out/skip"
      '';
    }
  )");
  const auto result = eval.expression(
      call, "(import ./. { inherit params inputs; }).api", true);
  ASSERT_FALSE(result.build_error) << result.log_tail;
  ASSERT_EQ(result.files.size(), 1u);
  EXPECT_EQ(result.files[0].path, "share/tsugumi/api/result");
  EXPECT_EQ(result.files[0].bytes, std::string("\x00\xff", 2));
  EXPECT_THROW(eval.expression(call, R"(
    { type = "derivation"; name = "fake";
      drvPath = "/nix/store/00000000000000000000000000000000-fake.drv"; }
  )",
                               true),
               std::runtime_error);
  auto failed = eval.expression(call, R"(
    let pkgs = import <dep> {};
    in pkgs.runCommand "expression-failure" {} ''
      echo expected-build-failure >&2
      exit 1
    ''
  )",
                                true);
  EXPECT_TRUE(failed.build_error);
  EXPECT_NE(failed.log_tail.find("expected-build-failure"), std::string::npos);
}

TEST_F(DefinitionEvaluation, ExpressionBuildBoundsDirectoryEntriesAndBytes) {
  call.projects.at("").emplace("dep", pins.at("nixpkgs"));
  NixEvaluator eval("args: { params = {}; inputs = {}; }");
  const auto entries = eval.expression(call, R"(
    let pkgs = import <dep> {};
    in pkgs.runCommand "expression-many-files" {} ''
      mkdir "$out"
      for ((i=0;i<2001;i++)); do touch "$out/$i"; done
    ''
  )",
                                       true);
  ASSERT_FALSE(entries.build_error) << entries.log_tail;
  EXPECT_TRUE(entries.truncated);
  EXPECT_EQ(entries.files.size(), 2000u);
  const auto bytes = eval.expression(call, R"(
    let pkgs = import <dep> {};
    in pkgs.runCommand "expression-large-file" {} ''
      mkdir "$out"
      truncate -s 8388608 "$out/result"
    ''
  )",
                                     true);
  ASSERT_FALSE(bytes.build_error) << bytes.log_tail;
  EXPECT_TRUE(bytes.truncated);
  EXPECT_TRUE(bytes.files.empty());
}

}  // namespace
}  // namespace tsugumi

auto main(int argc, char** argv) -> int {
  tsugumi::NixEvaluator::run_subprocess_if_requested(argc, argv);
  if (auto result = tsugumi::NixEvaluator::build_client(argc, argv))
    return *result;
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
