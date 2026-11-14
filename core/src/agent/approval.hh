#pragma once

#include <algorithm>
#include <functional>
#include <set>
#include <vector>

#include "agent/json.hh"

namespace tsugumi::agent {

// All descriptions of existing targets come from authenticated project reads.
// Model text is shown only as the proposed arguments, never as evidence.
inline auto DescribeOperation(
    const std::string &name, json::Members args, const std::string &project,
    const std::function<json::Members(const std::string &,
                                      const json::Members &)> &read,
    std::vector<std::string> *run_ids = nullptr)
    -> std::string {
  if (run_ids) run_ids->clear();
  json::Members targets{{"project", json::Quote(project)}};
  if (name == "run_history_delete") {
    std::vector<std::string> runs;
    std::string cursor;
    const auto cutoff = json::Get(args, "ended_before");
    do {
      json::Members query{{"limit", "500"}};
      if (!cursor.empty())
        query["cursor"] = json::Quote(cursor);
      const auto page = read("run_history", query);
      for (const auto &row : json::Array(json::Get(page, "runs", "[]"))) {
        const auto status =
            json::Object(json::Get(json::Object(row), "status"));
        const auto state = json::Text(status, "state");
        const auto ended = json::Get(status, "ended_at");
        if ((state == "succeeded" || state == "failed" ||
             state == "cancelled") &&
            (cutoff == "null" ||
             (ended != "null" && std::stoll(json::String(ended, ended)) <
                                     std::stoll(json::String(cutoff, cutoff))))) {
          runs.push_back(json::Encode(status));
          const auto id = json::Text(status, "run_id");
          if (id.empty()) throw std::runtime_error("approval target has no run ID");
          if (run_ids) run_ids->push_back(json::Quote(id));
        }
      }
      const auto next = json::Text(page, "next_cursor");
      if (!next.empty() && next == cursor)
        throw std::runtime_error("run history cursor did not advance");
      cursor = next;
    } while (!cursor.empty());
    std::sort(runs.begin(), runs.end());
    if (run_ids) std::sort(run_ids->begin(), run_ids->end());
    targets["runs"] = json::Encode(runs);
    targets["run_count"] = std::to_string(runs.size());
    targets["removes"] = json::Quote(
        "Run records, values, logs, kept files and scratch, including the "
        "project's history copy; pending remote results are retained.");
  } else if (name == "project_cache_clear" || name == "run_scratch_delete") {
    const auto usage = read("project_cache_usage", {});
    const auto current = json::Text(usage, "current_project");
    json::Members selected;
    for (const auto &row : json::Array(json::Get(usage, "projects", "[]"))) {
      const auto item = json::Object(row);
      if (json::Text(item, "project") == current)
        selected = item;
    }
    targets["cache"] = json::Quote(current);
    targets["name"] = json::Get(selected, "name");
    targets["measurement_error"] =
        json::Get(selected, "error", json::Get(usage, "error"));
    if (name == "project_cache_clear") {
      targets["build_bytes"] = json::Get(selected, "build_bytes");
      targets["removes"] = json::Quote(
          "This project's Nix build roots and unused build paths. Shared or "
          "still referenced paths are retained; run data and scratch are kept. "
          "null bytes means the size has not been measured.");
    } else {
      std::vector<std::string> folders;
      std::set<std::string> paths;
      for (const auto &path : json::Array(json::Get(args, "paths", "[]")))
        paths.insert(json::String(path));
      for (const auto &path : paths) {
        json::Members folder{{"path", json::Quote(path)}, {"bytes", "null"}};
        for (const auto &row :
             json::Array(json::Get(selected, "run_scratch", "[]"))) {
          const auto item = json::Object(row);
          if (json::Text(item, "path") == path)
            folder = item;
        }
        folders.push_back(json::Encode(folder));
      }
      targets["folders"] = json::Encode(folders);
      targets["protected_run"] = json::Get(usage, "protected_run");
      targets["removes"] = json::Quote(
          "Only these scratch folders; run records and kept outputs remain. "
          "The latest run, active runs and pending remote results are "
          "protected. null bytes means the size has not been measured.");
    }
  } else {
    const auto snapshot = read("project_inspect", {});
    const auto graph = read("version_graph", {{"limit", "5000"}});
    targets["revision"] = json::Get(snapshot, "rev");
    targets["head"] = json::Get(graph, "head");
    targets["dirty"] = json::Get(graph, "dirty");
    targets["versioning_enabled"] = json::Get(graph, "enabled");
    targets["project_name"] = json::Get(
        json::Object(json::Text(snapshot, "project_json", "{}")), "name");
    if (name == "version_checkout") {
      const auto requested = json::Text(args, "commit");
      std::vector<json::Members> matches;
      for (const auto &row : json::Array(json::Get(graph, "commits", "[]"))) {
        const auto commit = json::Object(row);
        bool match = json::Text(commit, "id") == requested;
        // Require a full core-reported commit ID, so a moving ref cannot change
        // what an approved call affects. The model can read version_graph
        // first.
        if (match)
          matches.push_back(commit);
      }
      if (matches.size() != 1)
        throw std::runtime_error(
            "use a full commit ID returned by version_graph");
      targets["version"] = json::Encode(matches.front());
    }
    targets["effect"] = json::Quote(
        name == "version_checkout"
            ? "Commit the saved working project, then replace its files with "
              "the selected version."
        : name == "version_init"
            ? "Start versioning the saved project and keep its first commit."
        : "Commit the saved working project and keep the proposed named version.");
  }
  const auto details = "operation_approval\n" + name + "\n" +
                       json::Encode(targets) +
                       "\nProposed arguments: " + json::Encode(args);
  return details;
}

} // namespace tsugumi::agent
