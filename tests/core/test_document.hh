#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "tsugumi/document/document.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/util/json_object.hh"
#include "tsugumi/util/system.hh"

namespace tsugumi::test {

inline auto project_json(std::string_view fields = "{}") -> std::string {
  auto project = json::Object(initial_project_json("test", {}));
  for (auto&& [key, value] : json::Object(fields))
    project[key] = std::move(value);
  return json::Encode(project);
}

inline auto card_json(std::string_view fields) -> std::string {
  auto card = json::Object(fields);
  const auto type = json::Text(card, "type");
  card.try_emplace("symbol", json::Quote(type));
  if (type == "collect" || type == "merge" || type == "code" ||
      type == "input") {
    card.try_emplace("maxRuns", "1");
    card.try_emplace("recordIO", R"("project")");
  }
  if (type == "code" || type == "input") {
    card.try_emplace("inputs", "[]");
    card.try_emplace("reuseWorker", "false");
    card.try_emplace("timeoutSeconds", "0");
    card.try_emplace("buildTimeoutSeconds", "0");
    card.try_emplace("recordLogs", R"("project")");
    card.try_emplace(
        "sandbox",
        R"({"filesystem":true,"pid":true,"ipc":true,"identity":true,"limits":{"cpu":0,"memoryMiB":0}})");
    card.try_emplace("grants", R"({"network":false,"gui":false})");
    card.try_emplace("system", json::Quote(native_system()));
    card.try_emplace("workerCapabilities", "[]");
  }
  return json::Encode(card);
}

inline auto readme(std::string_view title, std::string_view description = {})
    -> std::string {
  return "---\ntitle: " + std::string(title) +
         "\ndescription: " + std::string(description) + "\n---\n";
}

inline auto write_card(const std::filesystem::path& path, std::string_view json)
    -> void {
  fs::write_atomic(path, json);
  const auto note = path.parent_path() / "README.md";
  if (!std::filesystem::exists(note))
    fs::write_atomic(note, readme(path.parent_path().filename().string()));
}

}  // namespace tsugumi::test
