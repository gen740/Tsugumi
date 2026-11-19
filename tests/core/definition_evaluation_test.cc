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

namespace tsugumi {
namespace {

TEST(DefinitionEvaluationKey, SeparatesAllExplicitEvaluatorArguments) {
  NixCall original;
  original.pin_url = "https://github.com/NixOS/nixpkgs/archive/revision.tar.gz";
  original.pin_hash = "sha256-pin";
  original.system = "aarch64-darwin";
  // The actual runner supplies content-addressed paths here. A change to
  // sources/locks, contract/Nix params, or upstream definitions/flake products
  // changes its corresponding path before consulting the evaluation cache.
  original.args =
      "{ sdk = /sdk; card = /defs/digest; gen = /gen/digest; "
      "upstream = /up/digest; }";
  const auto key = original.evaluation_key();
  for (const auto& component : {"pin-url", "pin-hash", "system", "args"}) {
    auto changed = original;
    const std::string name = component;
    if (name == "pin-url")
      changed.pin_url += "revision";
    else if (name == "pin-hash")
      changed.pin_hash += "hash";
    else if (name == "system")
      changed.system = "aarch64-linux";
    else
      changed.args =
          "{ sdk = /sdk; card = /defs/new; gen = /gen/new; upstream = /up/new; "
          "remote = true; }";
    EXPECT_NE(key, changed.evaluation_key()) << name;
  }
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
    std::filesystem::create_directories(root / "sdk");
    fs::write_atomic(root / "sdk" / "value", "first");
    const auto project = parse_project_json(initial_project_json("evaluation"));
    const auto pin = project.pkgs_path->value();
    const auto split = pin.find('#');
    call.pin_url = pin.substr(0, split);
    call.pin_hash = pin.substr(split + 1);
    call.system = native_system();
    call.args = "{ root = /. + \"" + root.string() + "/sdk\"; }";
    call.root_dir = root / "roots";
    call.deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(120);
  }
  auto TearDown() -> void override { std::filesystem::remove_all(root); }
  auto evaluator(const std::string& body) -> std::unique_ptr<NixEvaluator> {
    return std::make_unique<NixEvaluator>(
        "pkgs: args: { executionList = false; implementationCount = 1; "
        "manifest = pkgs.writeText \"evaluation-test\" (" +
            body + "); }",
        std::vector<std::string>{root.string()});
  }
  std::filesystem::path root;
  NixCall call;
};

TEST_F(DefinitionEvaluation, CancellationRemovesAnEvaluationBehindABusyOne) {
  auto eval = evaluator("builtins.readFile (args.root + \"/value\")");
  std::promise<void> entered, release;
  auto gate = release.get_future();
  auto busy_call = call;
  busy_call.on_phase = [&](bool) {
    entered.set_value();
    gate.wait();
  };
  auto busy = std::async(std::launch::async, [&] { eval->evaluate(busy_call); });
  entered.get_future().wait();

  std::stop_source stop;
  auto queued = call;
  queued.cancel = stop.get_token();
  auto waiting = std::async(std::launch::async, [&] {
    EXPECT_THROW(eval->evaluate(queued), NixInterrupted);
  });
  stop.request_stop();
  // Bound the deadlock assertion so a regression can still release the gate.
  const auto outcome = waiting.wait_for(std::chrono::seconds(5));
  release.set_value();
  waiting.get();
  busy.get();
  EXPECT_EQ(outcome, std::future_status::ready);
}

TEST_F(DefinitionEvaluation,
       UnchangedReadsHitAndSdkContentsInvalidateCachedImports) {
  fs::write_atomic(root / "sdk" / "default.nix", "builtins.readFile ./value");
  auto eval = evaluator("import (args.root + \"/default.nix\")");
  auto first = eval->realise_native(call, eval->evaluate(call));
  ASSERT_NE(first.evaluation_epoch, 0u);
  EXPECT_TRUE(eval->reuse(call, first));
  fs::write_atomic(root / "sdk" / "value", "second");
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
  fs::write_atomic(root / "sdk" / "optional", "added");
  EXPECT_FALSE(eval->reuse(call, first));
  auto added = eval->realise_native(call, eval->evaluate(call));
  EXPECT_NE(first.derivation.path, added.derivation.path);
  std::filesystem::remove(root / "sdk" / "optional");
  EXPECT_FALSE(eval->reuse(call, added));
  EXPECT_EQ(first.derivation.path, eval->evaluate(call).derivation.path);
}

TEST_F(DefinitionEvaluation, CopiedSdkTreeInvalidatesSourceToStoreCache) {
  fs::write_atomic(root / "sdk" / "views" / "line-chart.js", "first view");
  // Interpolation copies the tree into the store, as the SDK's derivation
  // inputs do. builtins.toString alone would only pass a local path string.
  auto eval = evaluator(R"("${args.root}")");
  auto first = eval->realise_native(call, eval->evaluate(call));
  ASSERT_NE(first.evaluation_epoch, 0u);
  EXPECT_TRUE(eval->reuse(call, first));
  fs::write_atomic(root / "sdk" / "views" / "line-chart.js", "second view");
  EXPECT_FALSE(eval->reuse(call, first));
  auto second = eval->evaluate(call);
  EXPECT_NE(first.derivation.path, second.derivation.path);
}

TEST_F(DefinitionEvaluation,
       TargetSystemChangesTheDerivationWithoutBuildingOutputs) {
  auto eval = evaluator("pkgs.stdenv.hostPlatform.system");
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
  fs::write_atomic(root / "sdk" / "value", "second");
  EXPECT_FALSE(eval->reuse(call, first));
  auto second = eval->realise_native(call, eval->evaluate(call));
  EXPECT_EQ(fs::read_file(second.derivation.output).value(),
            body.substr(0, body.size() - 5) + "second");
}

}  // namespace
}  // namespace tsugumi
