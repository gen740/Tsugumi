#include "version_facts.hh"

#include "version_record.hh"
namespace tsugumi::version_facts {
namespace R = Tsugumi::RPC;
auto commit(const std::string& raw) -> std::unique_ptr<R::VersionCommitFactsT> {
  version_record::Commit record;
  if (schema::decode(raw, record) || *record.format != "commit") return {};
  if (*record.kind != "create" && *record.kind != "checkout" &&
      *record.kind != "run")
    return {};
  auto out = std::make_unique<R::VersionCommitFactsT>();
  out->kind = *record.kind;
  out->card = *record.card;
  return out;
}
auto run(const std::string& raw) -> std::unique_ptr<R::VersionRunFactsT> {
  if (raw.empty()) return {};
  version_record::Run record;
  if (auto error = schema::decode(raw, record))
    throw std::runtime_error(error->message);
  auto out = std::make_unique<R::VersionRunFactsT>();
  out->whole = *record.whole;
  out->card = *record.card;
  out->propagate = *record.propagate;
  out->saved_inputs = *record.saved_inputs;
  for (const auto& count : *record.ran) {
    auto value = std::make_unique<R::VersionInvocationCountT>();
    value->card = *count.card;
    value->count = *count.count;
    out->ran.push_back(std::move(value));
  }
  for (const auto& failure : *record.failed) {
    auto value = std::make_unique<R::VersionRunFailureT>();
    value->card = *failure.card;
    value->kind = *failure.kind;
    out->failures.push_back(std::move(value));
  }
  out->never = *record.never;
  return out;
}
auto changes(const std::string& raw) -> R::VersionChangesT {
  version_record::Changes record;
  if (auto error = schema::decode(raw, record))
    throw std::runtime_error(error->message);
  R::VersionChangesT out;
  out.added = *record.added;
  out.removed = *record.removed;
  out.workflow = *record.workflow;
  for (const auto& change : *record.changed) {
    auto value = std::make_unique<R::VersionCardChangeT>();
    value->card = *change.card;
    value->kinds = *change.kinds;
    out.changed.push_back(std::move(value));
  }
  return out;
}
}  // namespace tsugumi::version_facts
