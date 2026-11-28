#pragma once

#include <functional>
#include <set>
#include <stdexcept>
#include <vector>

#include "tsugumi/util/json_object.hh"

namespace tsugumi::agent {

// All descriptions of existing targets come from authenticated project reads.
// Model text is shown only as the proposed arguments, never as evidence.
inline auto DescribeOperation(
    const std::string& name, json::Members args, const std::string& project,
    const std::function<json::Members(const std::string&,
                                      const json::Members&)>& read)
    -> std::string {
  if (name != "run_scratch_delete")
    throw std::runtime_error("no decision describes " + name);
  json::Members targets{{"project", json::Quote(project)}};
  const auto usage = read("project_cache_usage", {});
  const auto current = json::Text(usage, "current_project");
  json::Members selected;
  for (const auto& row : json::Array(json::Get(usage, "projects", "[]"))) {
    const auto item = json::Object(row);
    if (json::Text(item, "project") == current) selected = item;
  }
  targets["cache"] = json::Quote(current);
  targets["name"] = json::Get(selected, "name");
  targets["measurement_error"] =
      json::Get(selected, "error", json::Get(usage, "error"));
  std::vector<std::string> folders;
  std::set<std::string> paths;
  for (const auto& path : json::Array(json::Get(args, "paths", "[]")))
    paths.insert(json::String(path));
  for (const auto& path : paths) {
    json::Members folder{{"path", json::Quote(path)}, {"bytes", "null"}};
    for (const auto& row :
         json::Array(json::Get(selected, "run_scratch", "[]"))) {
      const auto item = json::Object(row);
      if (json::Text(item, "path") == path) folder = item;
    }
    folders.push_back(json::Encode(folder));
  }
  targets["folders"] = json::Encode(folders);
  targets["protected_run"] = json::Get(usage, "protected_run");
  targets["removes"] = json::Quote(
      "Only these scratch folders; run records and kept outputs remain. "
      "The latest run, active runs and pending remote results are "
      "protected. null bytes means the size has not been measured.");
  return name + "\n" + json::Encode(targets) +
         "\nProposed arguments: " + json::Encode(args);
}

}  // namespace tsugumi::agent
