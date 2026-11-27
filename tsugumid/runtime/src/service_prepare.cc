// Build every executable Code Card without running any.
#include <algorithm>
#include <memory>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "execution_service.hh"
#include "nix.hh"
#include "service.hh"
#include "tsugumi/protocol/runtime_state_generated.h"
#include "tsugumi/util/flat_file.hh"
#include "tsugumi/util/json_object.hh"
#include "tsugumi/util/log.hh"

namespace tsugumi {
namespace {

constexpr std::string_view kNativeExportSystem = "aarch64-darwin";

auto code_card(const Node& node) -> bool {
  return node.kind == CardType::Code && !node.is_virtual;
}

auto build_request(const Node& node, const DocumentPtr& doc,
                   const std::string& card) -> BuildRequest {
  BuildRequest request;
  request.node = &node;
  request.doc = doc;
  request.timeout_seconds = node.settings.build_timeout_seconds;
  request.log = [card](auto chunk) {
    for (const auto& line : chunk.lines)
      log::info("build " + card + ": " + line);
    if (chunk.truncated) log::info("build " + card + ": log truncated");
  };
  return request;
}

auto strings(const auto& values) -> std::string {
  std::vector<std::string> quoted;
  for (const auto& value : values) quoted.push_back(json::Quote(value));
  return json::Encode(quoted);
}

auto export_host_paths(const Document& doc) -> std::set<std::string> {
  std::set<std::string> names;
  const auto collect = [&](const auto& project) {
    if (const auto* paths = project.hostPaths())
      for (const auto* path : *paths) names.insert(path->name()->str());
  };
  collect(doc.project());
  for (const auto& node : doc.resolution->nodes) {
    if (!node.is_virtual) continue;
    const auto file =
        node.project_dir + "externals/" + node.path.back() + "/tsugumi.json";
    const auto nested =
        parse_external_project(file, *doc.sources.at(file).document);
    collect(document_text::root<Tsugumi::Document::Project>(nested.buffer));
  }
  return names;
}

}  // namespace

auto Service::prepare_cards(std::ostream& out) -> bool {
  const auto doc = store_->current();
  const auto& graph = *doc->resolution;
  bool all = true;
  Tsugumi::State::PreparedCardsT manifest;
  for (const auto& node : graph.nodes) {
    const auto card = path_key(node.path);
    if (!code_card(node)) continue;
    auto request = build_request(node, doc, card);
    Tsugumi::State::PreparedCardT card_record;
    card_record.card = card;
    try {
      // The same evaluation and build as a run of this Card on the local
      // service, whichever branches a run would take.
      auto execution = execution_service_->prepare(node, run_scratch_, request);
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
      card_record.error = f.failure.diagnostic.value_or(f.what());
    } catch (const std::exception& e) {
      all = false;
      card_record.error = e.what();
    }
    manifest.cards.push_back(std::make_unique<Tsugumi::State::PreparedCardT>(
        std::move(card_record)));
  }
  const auto bytes = flat::encode<Tsugumi::State::PreparedCards>(manifest);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return all && out.good();
}

auto Service::prepare_native_cards() -> std::optional<std::string> {
  const auto doc = store_->current();
  const auto& graph = *doc->resolution;
  bool valid = true;
  for (const auto& diagnostic : graph.all_diagnostics(true)) {
    if (!diagnostic.error) continue;
    const std::string scope = diagnostic.card
                                  ? "Card " + path_key(*diagnostic.card)
                                  : diagnostic.path.value_or("project");
    log::error(scope + ": " + diagnostic.message);
    valid = false;
  }
  for (const auto& node : graph.nodes) {
    if (code_card(node) &&
        std::ranges::find(node.settings.systems, kNativeExportSystem) ==
            node.settings.systems.end()) {
      log::error("Card " + path_key(node.path) + " must declare " +
                 std::string(kNativeExportSystem) + " in systems");
      valid = false;
    }
  }
  if (!valid) return std::nullopt;

  const SharedStoreDerivationTransport transport;
  std::vector<std::string> cards;
  for (const auto& node : graph.nodes) {
    if (!code_card(node)) continue;
    const auto card = path_key(node.path);
    auto request = build_request(node, doc, card);
    request.target = {WorkerKind::native, std::string(kNativeExportSystem)};
    request.transport = &transport;
    try {
      const auto plan = nix_->build(request);
      const auto& build = plan.common;
      json::Members record{
          {"card", json::Quote(card)},
          {"plan", json::Quote(build.derivation.output)},
          {"closure", strings(build.environment.paths)},
          {"system", json::Quote(build.derivation.system)},
          {"reuse_worker", node.settings.reuse_worker ? "true" : "false"},
          {"execution_list", plan.execution_list ? "true" : "false"},
          {"implementation_labels", strings(plan.implementation_labels)},
      };
      if (doc->sources.contains(node.def_dir + "go.mod"))
        record.emplace("language", json::Quote("go"));
      cards.push_back(json::Encode(record));
    } catch (const Failed& failure) {
      log::error("Card " + card + ": " +
                 failure.failure.diagnostic.value_or(failure.what()));
      valid = false;
    } catch (const std::exception& failure) {
      log::error("Card " + card + ": " + failure.what());
      valid = false;
    }
  }
  if (!valid) return std::nullopt;
  return json::Encode(json::Members{
             {"version", "1"},
             {"system", json::Quote(kNativeExportSystem)},
             {"host_paths", strings(export_host_paths(*doc))},
             {"cards", json::Encode(cards)},
         }) +
         "\n";
}

}  // namespace tsugumi
