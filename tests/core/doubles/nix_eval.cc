#include "tsugumi/workerd/card_plan.hh"
#include "tsugumi/workerd/store.hh"
// A build product double, not a Nix interpreter. The real daemon, RPC,
// document store, scheduler and worker processes stay in use. Tests submit
// ordinary projects and assert their public RPC results; only this adapter
// knows how the core reaches the external evaluator.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "nix_eval.hh"
#include "nix_params.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/util/json.hh"

namespace tsugumi {
namespace {

struct BuildPlan {
  schema::Opt<"product", schema::Json> product;
  schema::Opt<"executionList", schema::v::Bool> execution_list;
  schema::Opt<"gate", schema::v::String> gate;
  schema::Opt<"error", schema::v::String> error;
  schema::Opt<"log", schema::v::String> log;
  schema::Opt<"cached", schema::v::Bool> cached;
  schema::Opt<"closure", schema::v::Array<schema::v::String>> closure;
  // Run list labels, one per implementation ("" for none); default: none.
  schema::Opt<"labels", schema::v::Array<schema::v::String>> labels;
};

struct ViewsOnlyPlan {
  schema::Opt<"runs", schema::v::Array<schema::v::String>> runs;
  schema::Opt<"views", schema::v::Array<schema::Obj<schema::CardView>>> views;
};

auto check_interrupted(const NixCall& call) -> void {
  if (call.cancel.stop_requested())
    throw NixInterrupted(false, "mock build cancelled");
  if (call.deadline && std::chrono::steady_clock::now() >= *call.deadline)
    throw NixInterrupted(true, "mock build exceeded its deadline");
}

auto valid_gate(const std::string& name) -> bool {
  return !name.empty() &&
         std::all_of(name.begin(), name.end(), [](unsigned char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_';
         });
}

auto plan_text(const BuildPlan& plan, const NixCall& call, NixResult& result)
    -> std::string {
  const auto* value = schema::get(plan.product);
  if (!value)
    throw std::runtime_error("mock-build.json needs product or error");
  schema::CardPlan product;
  if (auto error = schema::decode_card_plan(schema::text(*value), product))
    throw std::runtime_error("mock Card plan is invalid: " + error->message);
  if (call.views_only) {
    // The view-only plan has no runs and no worker.
    ViewsOnlyPlan views_only;
    views_only.runs = product.runs;
    views_only.runs->value().clear();
    views_only.views = std::move(product.views);
    result.execution_list = true;
    result.implementation_count = 0;
    return schema::encode(views_only);
  }
  result.execution_list =
      schema::get(plan.execution_list) && *schema::get(plan.execution_list);
  result.implementation_count =
      static_cast<uint32_t>(product.runs->value().size());
  if (!result.execution_list && result.implementation_count != 1)
    throw std::runtime_error("mock scalar Card plan needs exactly one run");
  enforce_implementation_limit(result.implementation_count,
                               call.max_implementations);
  if (const auto* labels = schema::get(plan.labels))
    result.implementation_labels = *labels;
  else
    result.implementation_labels.assign(result.implementation_count, "");
  check_implementation_labels(result.implementation_labels,
                              result.implementation_count);
  return schema::text(*value);
}

}  // namespace

struct NixEvaluator::Impl {
  std::filesystem::path control;
  std::filesystem::path products;
  uint64_t full_evaluations = 0;
  // Evaluations take turns, as on the real evaluator's single thread.
  std::mutex evaluation;
};

auto NixEvaluator::init_process() -> void {}

auto NixEvaluator::run_subprocess_if_requested(int, char**) -> void {}

auto NixEvaluator::build_client(int, char**) -> std::optional<int> {
  return std::nullopt;
}
auto NixStore::init_process() -> void {}

// Tests that serve view files name the folder standing in for the store.
auto NixStore::store_dir() -> std::string {
  const char* dir = std::getenv("TSUGUMI_MOCK_STORE");
  return dir && *dir ? dir : "/nix/store";
}

auto NixStore::substituters() -> std::vector<std::string> { return {}; }

auto NixStore::closure(const std::string& path) -> std::vector<std::string> {
  return {path};
}

NixEvaluator::NixEvaluator(std::string) : impl_(std::make_unique<Impl>()) {
  const char* control = std::getenv("TSUGUMI_MOCK_CONTROL");
  if (!control || !*control)
    throw std::runtime_error("tsugumid-mock requires TSUGUMI_MOCK_CONTROL");
  if (!std::filesystem::path(control).is_absolute())
    throw std::runtime_error("TSUGUMI_MOCK_CONTROL must be an absolute path");
  std::filesystem::create_directories(control);
  impl_->control = std::filesystem::canonical(control);
  impl_->products = impl_->control / "products";
}

NixEvaluator::~NixEvaluator() = default;

auto NixEvaluator::evaluate(const NixCall& call) -> NixResult {
  if (call.system.empty())
    throw std::runtime_error("execution target system is unknown");
  std::lock_guard turn(impl_->evaluation);
  check_interrupted(call);
  if (call.definition_dir.empty() || !call.definition_dir.is_absolute() ||
      std::ranges::none_of(call.definitions, [&](const auto& scope) {
        return scope.directory == call.definition_dir;
      }))
    throw std::runtime_error(
        "mock build needs confirmed definition sources inside the evaluation "
        "area");

  // Test-only observation of full Card evaluations, including store hits.
  // Evaluations take turns (above). Publish before returning
  // so a completed RPC run is also a barrier for readers of this counter.
  if (!call.views_only)
    fs::write_atomic(impl_->control / "full-evaluations",
                     std::to_string(++impl_->full_evaluations), 0600);

  BuildPlan plan;
  const auto source = fs::read_beneath(call.definition_dir, "mock-build.json");
  if (auto error = schema::decode(source, plan))
    throw std::runtime_error("invalid mock-build.json: " + error->message);
  // Reject incomplete plans before announcing a gate. An error plan may omit
  // its product, while malformed products never fall back to real Nix.
  NixResult result;
  result.evaluation_epoch = 1;
  if (schema::get(plan.product))
    result.plan_json = plan_text(plan, call, result);
  if (!schema::get(plan.error) && result.plan_json.empty())
    throw std::runtime_error("mock-build.json needs product or error");
  if (call.on_phase) call.on_phase(false);
  if (const auto* line = schema::get(plan.log); line && call.log) {
    const std::string text = *line + "\n";
    call.log({std::span(&text, 1)});
  }
  if (const auto* gate = schema::get(plan.gate); gate && !call.views_only) {
    if (!valid_gate(*gate))
      throw std::runtime_error(
          "mock build gate must contain only letters, digits, '-' or '_'");
    const auto started = impl_->control / (*gate + ".started");
    const auto release = impl_->control / (*gate + ".release");
    fs::write_atomic(started, "started\n", 0600);
    while (!std::filesystem::exists(release)) {
      check_interrupted(call);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  check_interrupted(call);
  if (const auto* error = schema::get(plan.error))
    throw std::runtime_error(*error);

  const auto digest = crypto::sha256_hex(result.plan_json + call.system);
  const auto product = impl_->products / (digest + "-plan.json");
  const bool cached = schema::get(plan.cached)
                          ? *schema::get(plan.cached)
                          : std::filesystem::exists(product);
  result.derivation = {"/nix/store/00000000000000000000000000000000-mock.drv",
                       "/nix/store/00000000000000000000000000000000-mock",
                       call.system};
  result.built = !cached;
  if (const auto* closure = schema::get(plan.closure))
    result.closure = *closure;
  return result;
}

auto NixEvaluator::reuse(const NixCall&, const NixResult& result) -> bool {
  return fs::resolves_inside(impl_->products, result.derivation.output) &&
         std::filesystem::is_regular_file(result.derivation.output);
}

auto NixEvaluator::make_available(const NixCall& call,
                                  const DerivationClosureTransport& transport,
                                  const WorkerDerivation& derivation) -> void {
  check_interrupted(call);
  transport.send(derivation);
}

auto ArchiveDerivationTransport::send(const WorkerDerivation&) const -> void {
  fs::write_atomic(archive_, "test Nix derivation archive", 0600);
}

auto ArchiveDerivationTransport::receive(nix::Store&,
                                         const WorkerDerivation&) const
    -> void {
  throw std::runtime_error("mock evaluator cannot import a derivation");
}

auto NixEvaluator::realise_native(const NixCall& call, NixResult result)
    -> NixResult {
  const auto digest = crypto::sha256_hex(result.plan_json + call.system);
  const auto product = impl_->products / (digest + "-plan.json");
  if (result.built && call.on_phase) call.on_phase(true);
  fs::write_atomic(product, result.plan_json, 0444);
  result.derivation.output = product.string();
  result.derivation.path = product.string() + ".drv";
  result.closure.push_back(product.string());
  result.closure.push_back(product.string() + "-dependency");
  std::ranges::sort(result.closure);
  result.closure.erase(
      std::unique(result.closure.begin(), result.closure.end()),
      result.closure.end());
  if (!call.root_dir.empty()) {
    std::filesystem::create_directories(call.root_dir);
    const auto root =
        call.root_dir / (call.root_prefix + product.filename().string());
    auto temporary = root;
    temporary += ".tmp-" + crypto::random_token(6);
    std::filesystem::create_symlink(product, temporary);
    std::filesystem::rename(temporary, root);
  }
  return result;
}

auto NixEvaluator::lock_source(const std::string&) -> NixSource {
  throw std::runtime_error("the mock evaluator cannot fetch sources");
}

auto NixEvaluator::expression(const NixCall&, const std::string&, bool)
    -> ExpressionResult {
  throw std::runtime_error("the mock evaluator cannot evaluate expressions");
}

auto NixEvaluator::check_params_value(const NixCall&, std::string_view text,
                                      const std::string& origin) -> std::string {
  return evaluate_literal_params(
      std::filesystem::path(origin).parent_path().generic_string() + '/', text);
}

auto NixStore::rooted_usage(const std::vector<std::vector<std::string>>& roots)
    -> std::vector<std::map<std::string, uint64_t>> {
  std::vector<std::map<std::string, uint64_t>> result;
  for (const auto& project : roots) {
    auto& sizes = result.emplace_back();
    for (const auto& path : project)
      sizes.emplace(path, std::filesystem::file_size(path));
  }
  return result;
}

}  // namespace tsugumi
