#include "version_facts.hh"

#include <stdexcept>

#include "tsugumi/util/flat_file.hh"

namespace tsugumi::version_facts {
namespace R = Tsugumi::RPC;

auto commit(const std::string& raw) -> std::unique_ptr<R::VersionCommitFactsT> {
  auto out = std::make_unique<R::VersionCommitFactsT>();
  const auto text = std::string_view(raw).substr(0, raw.find('\n'));
  constexpr std::string_view run_prefix = "Run Tsugumi card ";
  if (text == "Initialize Tsugumi project")
    out->kind = "create";
  else if (text == "Restore Tsugumi version")
    out->kind = "checkout";
  else if (text.starts_with(run_prefix)) {
    out->kind = "run";
    out->card = text.substr(run_prefix.size());
  } else
    return {};
  return out;
}

auto run(const std::string& raw) -> std::unique_ptr<R::VersionRunFactsT> {
  if (raw.empty()) return {};
  auto record = flat::decode<R::VersionRunFacts>(raw);
  if (!record) throw std::runtime_error("invalid Git run facts");
  return std::make_unique<R::VersionRunFactsT>(std::move(*record));
}

auto changes(const std::string& raw) -> R::VersionChangesT {
  auto record = flat::decode<R::VersionChanges>(raw);
  if (!record) throw std::runtime_error("invalid Git change facts");
  return std::move(*record);
}
}  // namespace tsugumi::version_facts
