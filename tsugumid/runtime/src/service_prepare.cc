// Building every Card of the project without running any (tsugumid
// --prepare-cards), for a tool that packages the built project. A core
// started with --prepared-cards then runs them without evaluating.
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "execution_service.hh"
#include "nix.hh"
#include "service.hh"
#include "tsugumi/protocol/runtime_state_generated.h"
#include "tsugumi/util/flat_file.hh"
#include "tsugumi/util/log.hh"

namespace tsugumi {

auto Service::prepare_cards(std::ostream& out) -> bool {
  const auto doc = store_->current();
  const auto& graph = *doc->resolution;
  bool all = true;
  Tsugumi::State::PreparedCardsT manifest;
  for (const auto& node : graph.nodes) {
    const auto card = path_key(node.path);
    if (node.kind != CardType::Code || node.is_virtual) continue;
    BuildRequest request;
    request.node = &node;
    request.doc = doc;
    request.timeout_seconds = node.settings.build_timeout_seconds;
    request.log = [&](auto chunk) {
      for (const auto& line : chunk.lines)
        log::info("build " + card + ": " + line);
      if (chunk.truncated) log::info("build " + card + ": log truncated");
    };
    Tsugumi::State::PreparedCardT card_record;
    card_record.card = card;
    try {
      // The same evaluation and build as a run of this Card on the local
      // service, whichever branches a run would take.
      auto execution = execution_service(kLocalExecutionService)
                           .prepare(node, run_scratch_, request);
      const auto plan = nix_->build(request);
      card_record.closure = plan.common.environment.paths;
      card_record.key = plan.evaluation_key;
      card_record.derivation = plan.common.derivation.path;
      card_record.plan = plan.common.derivation.output;
      card_record.system = plan.common.derivation.system;
      card_record.execution_list = plan.execution_list;
      card_record.implementations = plan.implementations.size();
      card_record.implementation_labels = plan.implementation_labels;
    } catch (const Failed& f) {
      all = false;
      card_record.error = f.failure.diagnostic.value_or("");
    } catch (const std::exception& e) {
      all = false;
      card_record.error = e.what();
    }
    manifest.cards.push_back(
        std::make_unique<Tsugumi::State::PreparedCardT>(std::move(card_record)));
  }
  const auto bytes = flat::encode<Tsugumi::State::PreparedCards>(manifest);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return all && out.good();
}

}  // namespace tsugumi
