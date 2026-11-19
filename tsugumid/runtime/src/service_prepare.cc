// Building every Card of the project without running any (tsugumid
// --prepare-cards), for a tool that packages the built project. A core
// started with --prepared-cards then runs them without evaluating.
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "execution_service.hh"
#include "nix.hh"
#include "service.hh"
#include "tsugumi/util/json.hh"
#include "tsugumi/util/log.hh"

namespace tsugumi {

auto Service::prepare_cards(std::ostream& out) -> bool {
  const auto doc = store_->current();
  const auto& graph = *doc->resolution;
  bool all = true;
  for (const auto& node : graph.nodes) {
    if (node.kind != Template::Code || node.is_virtual) continue;
    const auto card = path_key(node.path);
    BuildRequest request;
    request.node = &node;
    request.doc = doc;
    request.timeout_seconds = node.settings.build_timeout_seconds;
    request.log = [&](auto lines) {
      for (const auto& line : lines) log::info("build " + card + ": " + line);
    };
    // One schema::PreparedCard per line.
    std::vector<std::pair<std::string, hosq::json::GenericDom<>>> fields;
    fields.emplace_back("card", schema::string_value(card));
    try {
      // The same evaluation and build as a run of this Card on the local
      // service, whichever branches a run would take.
      auto execution = execution_service(kLocalExecutionService)
                           .prepare(node, run_scratch_, request);
      const auto plan = nix_->build(request);
      std::string closure = "[";
      for (const auto& path : plan.common.environment.paths)
        closure += (closure.size() > 1 ? "," : "") +
                   schema::text(schema::string_value(path));
      fields.emplace_back("key", schema::string_value(plan.evaluation_key));
      fields.emplace_back("derivation",
                          schema::string_value(plan.common.derivation.path));
      fields.emplace_back("manifest",
                          schema::string_value(plan.common.derivation.output));
      fields.emplace_back("system",
                          schema::string_value(plan.common.derivation.system));
      fields.emplace_back(
          "execution_list",
          schema::capture(plan.execution_list ? "true" : "false"));
      fields.emplace_back(
          "implementations",
          schema::capture(std::to_string(plan.implementations.size())));
      fields.emplace_back("closure", schema::capture(closure + "]"));
    } catch (const Failed& f) {
      all = false;
      fields.emplace_back(
          "error", schema::string_value(f.failure.diagnostic.value_or("")));
    } catch (const std::exception& e) {
      all = false;
      fields.emplace_back("error", schema::string_value(e.what()));
    }
    std::vector<std::pair<std::string, const hosq::json::GenericDom<>*>>
        members;
    for (const auto& [name, value] : fields) members.emplace_back(name, &value);
    out << schema::object_text(members) << '\n';
    out.flush();
  }
  return all;
}

}  // namespace tsugumi
