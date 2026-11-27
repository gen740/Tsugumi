// The offline launcher has only the bundled build plans.
#include "nix_eval.hh"
#include "nix_params.hh"
#include "tsugumi/workerd/cache_relay_store.hh"
#include "tsugumi/workerd/store.hh"

namespace tsugumi {
namespace {

[[noreturn]] auto unavailable() -> void {
  throw std::runtime_error(
      "Nix evaluation and store operations are unavailable in an exported app");
}

}  // namespace

struct NixEvaluator::Impl {};

NixEvaluator::NixEvaluator(std::string) { unavailable(); }
NixEvaluator::~NixEvaluator() = default;

auto NixEvaluator::evaluate(const NixCall&) -> NixResult { unavailable(); }
auto NixEvaluator::reuse(const NixCall&, const NixResult&) -> bool {
  unavailable();
}
auto NixEvaluator::make_available(const NixCall&,
                                  const DerivationClosureTransport&,
                                  const WorkerDerivation&) -> void {
  unavailable();
}
auto NixEvaluator::realise_native(const NixCall&, NixResult) -> NixResult {
  unavailable();
}
auto NixEvaluator::expression(const NixCall&, const std::string&, bool)
    -> ExpressionResult {
  unavailable();
}
auto NixEvaluator::lock_source(const std::string&) -> NixSource {
  unavailable();
}

auto BinaryCacheDerivationTransport::send(const WorkerDerivation&) const
    -> void {
  unavailable();
}
auto BinaryCacheDerivationTransport::receive(nix::Store&,
                                             const WorkerDerivation&) const
    -> void {
  unavailable();
}
auto publish_to_relay_cache(const std::string&, const std::string&,
                            CacheRelay::Exchange) -> void {
  unavailable();
}

auto NixStore::store_dir() -> std::string { unavailable(); }
auto NixStore::substituters() -> std::vector<std::string> { unavailable(); }
auto NixStore::rooted_usage(const std::vector<std::vector<std::string>>&)
    -> std::vector<std::map<std::string, uint64_t>> {
  unavailable();
}

auto evaluate_literal_params(const std::string&, std::string_view)
    -> std::string {
  unavailable();
}
auto rewrite_param_literal(const std::string&, std::string_view,
                           std::string_view, std::string_view) -> std::string {
  unavailable();
}

}  // namespace tsugumi
