// Building every Card of the project without running any (tsugumid
// --prepare-cards), for a tool that packages the built project.
#include <ostream>

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
    auto key = schema::string_value(card);
    BuildRequest request;
    request.node = &node;
    request.doc = doc;
    request.jobs = opts_.env_cpus;
    request.cores = 1;
    request.timeout_seconds = node.settings.build_timeout_seconds;
    request.log = [&](const std::string& line) {
      log::info("build " + card + ": " + line);
    };
    try {
      // The same evaluation and build as a run of this Card on the local
      // service, whichever branches a run would take.
      auto prepared = execution_service(kLocalExecutionService)
                          .prepare(node, run_scratch_, request);
      const auto plan = nix_->build(request);
      auto manifest = schema::string_value(plan.common.key);
      auto derivation = schema::string_value(plan.common.derivation.path);
      auto system = schema::string_value(plan.common.derivation.system);
      out << schema::object_text({{"card", &key},
                                  {"manifest", &manifest},
                                  {"derivation", &derivation},
                                  {"system", &system}})
          << '\n';
    } catch (const Failed& f) {
      all = false;
      auto error = schema::string_value(f.failure.diagnostic.value_or(""));
      out << schema::object_text({{"card", &key}, {"error", &error}}) << '\n';
    } catch (const std::exception& e) {
      all = false;
      auto error = schema::string_value(e.what());
      out << schema::object_text({{"card", &key}, {"error", &error}}) << '\n';
    }
    out.flush();
  }
  return all;
}

}  // namespace tsugumi
