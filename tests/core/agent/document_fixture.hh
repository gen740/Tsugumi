#pragma once

#include "tsugumi/document/document.hh"
#include "tsugumi/util/json_object.hh"

namespace tsugumi::test {

inline auto FixtureProjectDocument(
    const std::string& overrides = "{}",
    const std::vector<std::string>& agent_exclude = {".git", ".DS_Store"})
    -> std::string {
  auto fields = json::Object(initial_project_json("fixture", agent_exclude));
  for (auto&& [key, value] : json::Object(overrides))
    fields[key] = std::move(value);
  return json::Encode(fields);
}

inline auto FixtureCodeCard(const std::string& overrides = "{}")
    -> std::string {
  auto fields = json::Object(R"({
    "type":"code","symbol":"chevron.left.forwardslash.chevron.right",
    "inputs":[],"maxRuns":1,"reuseWorker":false,"timeoutSeconds":0,
    "buildTimeoutSeconds":0,"recordLogs":"project","recordIO":"project",
    "sandbox":{"filesystem":true,"pid":true,"ipc":true,"identity":true,
               "limits":{"cpu":0,"memoryMiB":0}},
    "grants":{"network":false,"gui":false},
    "workerCapabilities":[]
  })");
#if defined(__aarch64__) || defined(__arm64__)
  fields["system"] = R"("aarch64-linux")";
#else
  fields["system"] = R"("x86_64-linux")";
#endif
  for (auto&& [key, value] : json::Object(overrides))
    fields[key] = std::move(value);
  return json::Encode(fields);
}

}  // namespace tsugumi::test
