// A build product double, not a Nix interpreter. The real daemon, RPC,
// document store, scheduler and worker processes stay in use. Tests submit
// ordinary projects and assert their public RPC results; only this adapter
// knows how the core reaches the external evaluator.
#include "nix_eval.hh"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "schema.hh"
#include "util/crypto.hh"
#include "util/fs.hh"

namespace tsugumi {
namespace {

struct BuildPlan {
  schema::Opt<"manifest", schema::Json> manifest;
  schema::Opt<"gate", schema::v::String> gate;
  schema::Opt<"error", schema::v::String> error;
  schema::Opt<"cached", schema::v::Bool> cached;
  schema::Opt<"closure", schema::v::Array<schema::v::String>> closure;
};

auto check_interrupted(const NixCall& call) -> void {
  if (call.cancel && call.cancel->load()) throw NixInterrupted(false, "mock build cancelled");
  if (call.deadline && std::chrono::steady_clock::now() >= *call.deadline)
    throw NixInterrupted(true, "mock build exceeded its deadline");
}

auto valid_gate(const std::string& name) -> bool {
  return !name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
  });
}

auto manifest_text(const BuildPlan& plan, const NixCall& call, NixResult& result) -> std::string {
  const auto* value = schema::get(plan.manifest);
  if (!value) throw std::runtime_error("mock-build.json needs manifest or error");
  schema::CardManifest manifest;
  if (auto error = schema::decode(*value, manifest))
    throw std::runtime_error("mock build manifest is invalid: " + error->message);
  const auto* run = schema::get(manifest.run);
  const auto* runs = schema::get(manifest.runs);
  const auto valid = [](const auto& body) {
    return (schema::get(body.bash) != nullptr) != (schema::get(body.package) != nullptr);
  };
  if ((run != nullptr) == (runs != nullptr) || (run && !valid(*run)) ||
      (runs && !std::ranges::all_of(*runs, valid)))
    throw std::runtime_error("mock build manifest needs one or more valid run implementations, or an empty run list");
  result.execution_list = runs != nullptr;
  result.implementation_count = runs ? static_cast<uint32_t>(runs->size()) : 1;
  if (result.implementation_count > call.max_implementations)
    throw NixImplementationLimit("input and run list expansion exceeds 256 combinations");
  return schema::text(*value);
}

}  // namespace

struct NixEvaluator::Impl {
  std::filesystem::path control;
  std::filesystem::path products;
  std::vector<std::string> allowed;
};

auto NixEvaluator::init_process() -> void {}

auto NixEvaluator::substituters() -> std::vector<std::string> { return {}; }

NixEvaluator::NixEvaluator(std::string, std::vector<std::string> allowed) : impl_(std::make_unique<Impl>()) {
  const char* control = std::getenv("TSUGUMI_MOCK_CONTROL");
  if (!control || !*control) throw std::runtime_error("tsugumid-mock requires TSUGUMI_MOCK_CONTROL");
  if (!std::filesystem::path(control).is_absolute())
    throw std::runtime_error("TSUGUMI_MOCK_CONTROL must be an absolute path");
  std::filesystem::create_directories(control);
  impl_->control = std::filesystem::canonical(control);
  impl_->products = impl_->control / "products";
  impl_->allowed = std::move(allowed);
}

NixEvaluator::~NixEvaluator() = default;

auto NixEvaluator::evaluate(const NixCall& call) -> NixResult {
  if (call.system.empty()) throw std::runtime_error("execution target system is unknown");
  check_interrupted(call);
  if (call.definition_dir.empty() || !call.definition_dir.is_absolute() ||
      std::none_of(impl_->allowed.begin(), impl_->allowed.end(), [&](const auto& root) {
        return fs::resolves_inside(root, call.definition_dir);
      }))
    throw std::runtime_error("mock build needs confirmed definition sources inside the evaluation area");

  BuildPlan plan;
  const auto source = fs::read_beneath(call.definition_dir, "mock-build.json");
  if (auto error = schema::decode(source, plan))
    throw std::runtime_error("invalid mock-build.json: " + error->message);
  // Reject incomplete plans before announcing a gate. An error plan may omit
  // its product, while malformed products never fall back to real Nix.
  NixResult result;
  std::string manifest;
  if (schema::get(plan.manifest)) manifest = manifest_text(plan, call, result);
  if (!schema::get(plan.error) && manifest.empty())
    throw std::runtime_error("mock-build.json needs manifest or error");
  if (call.on_phase) call.on_phase(false);
  if (const auto* gate = schema::get(plan.gate)) {
    if (!valid_gate(*gate)) throw std::runtime_error("mock build gate must contain only letters, digits, '-' or '_'");
    const auto started = impl_->control / (*gate + ".started");
    const auto release = impl_->control / (*gate + ".release");
    fs::write_atomic(started, "started\n", 0600);
    while (!std::filesystem::exists(release)) {
      check_interrupted(call);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  check_interrupted(call);
  if (const auto* error = schema::get(plan.error)) throw std::runtime_error(*error);

  const auto digest = crypto::sha256_hex(manifest + call.system);
  const auto product = impl_->products / (digest + "-manifest.json");
  const bool cached = schema::get(plan.cached) ? *schema::get(plan.cached) : std::filesystem::exists(product);
  result.derivation = {"/nix/store/00000000000000000000000000000000-mock.drv",
                       "/nix/store/00000000000000000000000000000000-mock", call.system};
  result.manifest = std::move(manifest);
  result.built = !cached;
  if (const auto* closure = schema::get(plan.closure)) result.closure = *closure;
  return result;
}

auto NixEvaluator::make_available(const NixCall& call, const DerivationClosureTransport& transport,
                                  const WorkerDerivation& derivation) -> void {
  check_interrupted(call);
  transport.send(derivation);
}

auto ArchiveDerivationTransport::send(const WorkerDerivation&) const -> void {
  fs::write_atomic(archive_, "test Nix derivation archive", 0600);
}

auto ArchiveDerivationTransport::receive(nix::Store&, const WorkerDerivation&) const -> void {
  throw std::runtime_error("mock evaluator cannot import a derivation");
}

auto NixEvaluator::realise_native(const NixCall& call, NixResult result) -> NixResult {
  const auto digest = crypto::sha256_hex(result.manifest + call.system);
  const auto product = impl_->products / (digest + "-manifest.json");
  if (result.built && call.on_phase) call.on_phase(true);
  fs::write_atomic(product, result.manifest, 0444);
  result.derivation.output = product.string();
  result.derivation.path = product.string() + ".drv";
  result.closure.push_back(product.string());
  result.closure.push_back(product.string() + "-dependency");
  std::ranges::sort(result.closure);
  if (!call.root_dir.empty()) {
    std::filesystem::create_directories(call.root_dir);
    result.root = call.root_dir / (call.root_prefix + product.filename().string());
    auto temporary = result.root;
    temporary += ".tmp-" + crypto::random_token(6);
    std::filesystem::create_symlink(product, temporary);
    std::filesystem::rename(temporary, result.root);
  }
  return result;
}

auto NixEvaluator::flake(const NixCall&, const std::string&) -> FlakeInfo {
  throw std::runtime_error("tsugumid-mock does not resolve flakes or contact the network");
}

auto NixEvaluator::valid(const std::string& store_path) -> bool {
  return fs::resolves_inside(impl_->products, store_path) && std::filesystem::is_regular_file(store_path);
}

auto NixEvaluator::rooted_usage(const std::vector<std::vector<std::string>>& roots)
    -> std::vector<std::map<std::string, uint64_t>> {
  std::vector<std::map<std::string, uint64_t>> result;
  for (const auto& project : roots) {
    auto& sizes = result.emplace_back();
    for (const auto& path : project) sizes.emplace(path, std::filesystem::file_size(path));
  }
  return result;
}

auto NixEvaluator::heap_bytes() -> uint64_t { return 0; }

}  // namespace tsugumi
