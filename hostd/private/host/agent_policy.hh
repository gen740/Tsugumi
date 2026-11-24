#pragma once

#include <set>
#include <string_view>

#include "tsugumi/util/json_object.hh"

namespace tsugumi::host {

inline const std::set<std::string> OwnedFields{"origin",
                                               "op_id",
                                               "save_intent",
                                               "approved_permissions",
                                               "run_ids",
                                               "restrict_to_run_ids",
                                               "check_permissions",
                                               "read_only",
                                               "hide_credentials",
                                               "no_project_socket",
                                               "transfers",
                                               "working_directory",
                                               "writable_paths",
                                               "mounts",
                                               "output_prefix",
                                               "workspace_only",
                                               "protected_paths",
                                               "excluded_paths"};
inline auto StripOwnedFields(std::string_view name, bool mutating,
                             json::Members& args) -> void {
  for (const auto& field : OwnedFields) {
    if (field == "op_id" && !mutating) continue;
    if ((field == "run_ids" || field == "restrict_to_run_ids") &&
        name != "run_history_delete")
      continue;
    args.erase(field);
  }
}

// Metadata presentation only: file/source bytes and arbitrary user JSON are
// opaque. Integrity hashes remain exact because mutations use them for CAS.
inline auto AgentReferences(const std::string& raw) -> std::string {
  namespace j = ::tsugumi::json;
  if (raw.starts_with('{')) {
    auto object = j::Object(raw);
    for (auto it = object.begin(); it != object.end();) {
      const auto& key = it->first;
      if (key == "execution_job_id") {
        it = object.erase(it);
        continue;
      }
      if (key == "commit" || key == "commit_id" || key == "head" ||
          key == "parents") {
        const auto shorten = [](const std::string& value) {
          const auto text = j::String(value);
          return text.size() == 40 &&
                         text.find_first_not_of("0123456789abcdef") == text.npos
                     ? j::Quote(text.substr(0, 12))
                     : value;
        };
        if (it->second.starts_with('[')) {
          auto values = j::Array(it->second);
          for (auto& value : values) value = shorten(value);
          it->second = j::Encode(values);
        } else
          it->second = shorten(it->second);
      } else if (key != "bytes" && key != "stdout_bytes" &&
                 key != "stderr_bytes" && key != "project_json" &&
                 key != "layout" && key != "card_json" && key != "readme" &&
                 key != "comments_json" && key != "text" &&
                 key != "record_json") {
        it->second = AgentReferences(it->second);
      }
      ++it;
    }
    // VersionCommit uses id, rather than commit. Recognize the typed shape.
    if (object.contains("author") && object.contains("message") &&
        object.contains("id")) {
      const auto id = j::Text(object, "id");
      if (id.size() == 40) object["id"] = j::Quote(id.substr(0, 12));
    }
    return j::Encode(object);
  }
  if (raw.starts_with('[')) {
    auto values = j::Array(raw);
    for (auto& value : values) value = AgentReferences(value);
    return j::Encode(values);
  }
  return raw;
}

// Only the host chooses the restrictions for model/plugin project calls.
inline auto RestrictAgentRequest(std::string_view name,
                                 ::tsugumi::json::Members& arguments) -> void {
  if (name == "project_save" || name == "project_reload")
    arguments["check_permissions"] = "true";
  if (name == "project_shell") {
    arguments.erase("origin");
    for (const auto* field :
         {"read_only", "hide_credentials", "no_project_socket"})
      arguments[field] = "true";
  }
}

}  // namespace tsugumi::host
