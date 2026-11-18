#include "host/agent_exclude.hh"

#include <algorithm>

#include "host/document_edits.hh"
#include "host/failure.hh"
#include "host/layout_saving.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/util/text.hh"

namespace tsugumi::host {
namespace {
namespace j = ::tsugumi::json;
auto Under(const std::filesystem::path& parent,
           const std::filesystem::path& child) -> bool {
  auto p = parent.begin(), c = child.begin();
  for (; p != parent.end(); ++p, ++c)
    if (c == child.end() || *c != *p) return false;
  return true;
}
auto Resolve(const std::filesystem::path& path) -> std::filesystem::path {
  std::error_code error;
  const auto result = std::filesystem::weakly_canonical(path, error);
  if (error) throw Failure("not_found", "Path not found");
  return result;
}
auto Bytes(const std::string& raw) -> std::string {
  if (raw.starts_with('"')) return j::String(raw);
  std::string bytes;
  if (raw.starts_with('{')) {
    if (!crypto::base64_decode(j::Text(j::Object(raw), "base64"), bytes))
      throw Failure("invalid_request", "Invalid file bytes");
  } else
    for (const auto& n : j::Array(raw)) bytes += static_cast<char>(j::UInt(n));
  return bytes;
}
}  // namespace

auto AgentExclude::Defaults() -> std::vector<std::string> {
  return {".git", ".DS_Store"};
}
auto AgentExclude::Entries(const std::string& document)
    -> std::vector<std::string> {
  const auto fields = j::Object(document);
  if (!fields.contains("agentExclude")) return Defaults();
  std::vector<std::string> entries;
  for (const auto& raw : j::Array(fields.at("agentExclude"))) {
    const auto path = j::String(raw);
    if (path.empty() || path.find('\0') != std::string::npos)
      throw Failure("invalid_document",
                    "agentExclude requires non-empty paths");
    entries.push_back(path);
  }
  return entries;
}
AgentExclude::AgentExclude(std::filesystem::path root,
                           const std::string& document)
    : root_(Resolve(std::filesystem::absolute(root))) {
  for (const auto& entry : Entries(document)) {
    const std::filesystem::path path(entry);
    paths_.push_back(Resolve(path.is_absolute() ? path : root_ / path));
  }
}
auto AgentExclude::Contains(const std::filesystem::path& path) const -> bool {
  const auto resolved = Resolve(path.is_absolute() ? path : root_ / path);
  return std::any_of(paths_.begin(), paths_.end(), [&](const auto& entry) {
    return Under(Resolve(entry), resolved);
  });
}
auto AgentExclude::Check(const std::filesystem::path& path) const -> void {
  if (Contains(path)) throw Failure("not_found", "Path not found");
}
auto AgentExclude::Card(const std::string& id, const std::string& prefix) const
    -> bool {
  const auto slash = id.find('/');
  const auto first = id.substr(0, slash);
  const auto definition = prefix + "definitions/" + first;
  if (Contains(definition)) return false;
  for (const auto* file : {"_card.json", "_link.json"})
    if (Contains(definition + '/' + file)) return false;
  if (slash == std::string::npos) return true;
  // A Card's id and its linked external folder need not have the same name.
  // Follow the actual definition, as the resolver and the helper boundary do.
  std::string external;
  try {
    external =
        j::Text(j::Object(fs::read_beneath(root_, definition + "/_link.json")),
                "external");
  } catch (const std::exception&) {
    return false;
  }
  if (!fs::valid_relative_path(external) ||
      external.find('/') != std::string::npos)
    return false;
  const auto nested = prefix + "externals/" + external + '/';
  return !Contains(nested) && Card(id.substr(slash + 1), nested);
}
auto AgentExclude::CheckRequest(const std::string& name,
                                const j::Members& args) const -> void {
  if (name.starts_with("project_file_")) Check(j::Text(args, "path"));
  if (name.starts_with("version_")) Check(".git");
  if (name.starts_with("run_") || name.starts_with("artifact_") ||
      name == "log_range") {
    Check(".tsugumi/runs.sqlite");
    if (args.contains("run_id"))
      Check(".tsugumi/runs/" + j::Text(args, "run_id"));
  }
  if (name == "module_read") {
    const std::filesystem::path module(j::Text(args, "module"));
    Check(module);
    Check(module / "index.js");
    Check(module / "view.wasm");
    Check(std::filesystem::path(".tsugumi/modules") / module.filename() /
          "index.js");
  }
  if (name == "artifact_read") {
    // Artifacts use run/Card-path/invocation/output as their canonical id.
    const auto id = j::Text(args, "artifact_id");
    const auto first = id.find('/'), last = id.rfind('/');
    const auto invocation = last == id.npos ? id.npos : id.rfind('/', last - 1);
    if (first != id.npos && invocation != id.npos && invocation > first) {
      if (!Card(id.substr(first + 1, invocation - first - 1)))
        throw Failure("not_found", "Artifact not found");
      const auto artifact =
          ".tsugumi/runs/" + id.substr(0, first) + "/artifacts/" + id;
      Check(artifact);
      if (args.contains("member"))
        Check(std::filesystem::path(artifact) / j::Text(args, "member"));
    }
  }
  for (const auto* key : {"card", "source_card", "trigger", "target"}) {
    if (!args.contains(key) || !args.at(key).starts_with('{')) continue;
    std::string id;
    for (const auto& segment :
         j::Array(j::Get(j::Object(args.at(key)), "segments", "[]"))) {
      if (!id.empty()) id += '/';
      id += j::String(segment);
    }
    if (!id.empty() && !Card(id)) throw Failure("not_found", "Card not found");
  }
}
auto AgentExclude::CheckContent(const std::string& raw) const -> void {
  if (raw.starts_with('[')) {
    for (const auto& value : j::Array(raw)) CheckContent(value);
  } else if (raw.starts_with('{')) {
    const auto object = j::Object(raw);
    CheckRequest("", object);
    for (const auto& [key, value] : object) {
      if (key == "path" && value.starts_with('"')) Check(j::String(value));
      CheckContent(value);
    }
  }
}
auto AgentExclude::ProjectText(const std::string& raw,
                               const std::string& prefix) const -> std::string {
  auto project = j::Object(raw);
  if (project.contains("canvas")) {
    auto graph = j::Object(project.at("canvas"));
    auto cards = j::Array(j::Get(graph, "cards", "[]"));
    std::erase_if(cards, [&](const auto& card) {
      return !Card(j::Text(j::Object(card), "id"), prefix);
    });
    graph["cards"] = j::Encode(cards);
    auto connections = j::Array(j::Get(graph, "connections", "[]"));
    std::erase_if(connections, [&](const auto& raw_wire) {
      const auto wire = j::Object(raw_wire);
      return !Card(j::Text(j::Object(j::Get(wire, "from", "{}")), "card"),
                   prefix) ||
             !Card(j::Text(j::Object(j::Get(wire, "to", "{}")), "card"),
                   prefix);
    });
    graph["connections"] = j::Encode(connections);
    project["canvas"] = j::Encode(graph);
  }
  if (project.contains("boundary")) {
    auto boundary = j::Object(project.at("boundary"));
    for (auto& [key, value] : boundary) {
      (void)key;
      auto ports = j::Object(value);
      std::erase_if(ports, [&](const auto& entry) {
        return !Card(j::Text(j::Object(entry.second), "card"), prefix);
      });
      value = j::Encode(ports);
    }
    project["boundary"] = j::Encode(boundary);
  }
  return j::Encode(project);
}
auto AgentExclude::Filter(const std::string& raw,
                          const std::string& prefix) const -> std::string {
  if (raw.starts_with('[')) {
    std::vector<std::string> values;
    for (const auto& value : j::Array(raw)) {
      if (value.starts_with('{')) {
        const auto object = j::Object(value);
        if (object.contains("path") && object.at("path").starts_with('"') &&
            Contains(j::Text(object, "path")))
          continue;
        if (object.contains("id") &&
            (object.contains("card_json") || object.contains("kind")) &&
            !Card(j::Text(object, "id"), prefix))
          continue;
        try {
          CheckRequest("", object);
        } catch (const Failure&) {
          continue;
        }
      }
      values.push_back(Filter(value, prefix));
    }
    return j::Encode(values);
  }
  if (!raw.starts_with('{')) return raw;
  auto object = j::Object(raw);
  if (object.contains("id") && object.contains("card_json")) {
    const auto dir = prefix + "definitions/" + j::Text(object, "id") + '/';
    if (Contains(dir + "_meta.json")) object.erase("meta_json");
  }
  for (auto& [key, value] : object) {
    if (key == "project_json")
      value = j::Quote(ProjectText(j::String(value), prefix));
    else if (key == "layout_json") {
      auto layout = j::Object(j::String(value));
      for (const auto* field : {"cards", "displays", "boundaries"})
        if (layout.contains(field)) {
          auto entries = j::Object(layout.at(field));
          std::erase_if(entries, [&](const auto& entry) {
            return !Card(entry.first, prefix);
          });
          layout[field] = j::Encode(entries);
        }
      value = j::Quote(j::Encode(layout));
    } else if (key != "bytes" && key != "text" && key != "record_json" &&
               key != "card_json" && key != "meta_json" && key != "link_json" &&
               key != "stdout_bytes" && key != "stderr_bytes")
      value = Filter(value, prefix);
  }
  return j::Encode(object);
}
auto AgentExclude::CheckSave(const j::Members& plan, const SaveBase& base) const
    -> void {
  const auto hidden_tree = [&](const std::string& path) {
    const auto resolved = Resolve(root_ / path);
    return Contains(path) ||
           std::any_of(paths_.begin(), paths_.end(), [&](const auto& entry) {
             return Under(resolved, Resolve(entry));
           });
  };
  const auto hidden_definition = [&](const std::string& dir) {
    if (hidden_tree(dir)) return true;
    const auto link = base.files.find(dir + "/_link.json");
    if (link == base.files.end()) return false;
    const auto prefix = dir.substr(0, dir.rfind("definitions/"));
    return hidden_tree(prefix + "externals/" +
                       j::Text(j::Object(link->second), "external"));
  };
  auto document = [&](const std::string& path, const std::string& bytes) {
    Check(path);
    const auto before = base.files.contains(path) ? base.files.at(path) : "{}";
    if (path.ends_with("/_link.json")) {
      const auto old = j::Object(before), after = j::Object(bytes);
      const auto external = j::Text(old, "external");
      const auto prefix = path.substr(0, path.rfind("definitions/"));
      if (!external.empty() && external != j::Text(after, "external") &&
          hidden_tree(prefix + "externals/" + external))
        throw Failure("not_found", "Card not found");
    }
    const bool project_document =
        path == "tsugumi.json" || IsExternalDocument(path, "tsugumi.json");
    const bool layout_document =
        path == "layout.json" || IsExternalDocument(path, "layout.json");
    if (!project_document && !layout_document) return;
    const auto prefix =
        path.substr(0, path.size() - (project_document ? 12 : 11));
    const auto old = j::Object(before), after = j::Object(bytes);
    if (path == "tsugumi.json" &&
        !DocumentsEqual(j::Get(old, "agentExclude", "null"),
                        j::Get(after, "agentExclude", "null")))
      throw Failure("permission_denied",
                    "Only a person can change agentExclude");
    if (project_document) {
      const auto old_graph = j::Object(j::Get(old, "canvas", "{}"));
      const auto new_graph = j::Object(j::Get(after, "canvas", "{}"));
      const auto cards = j::Array(j::Get(new_graph, "cards", "[]"));
      for (const auto& raw : j::Array(j::Get(old_graph, "cards", "[]"))) {
        const auto id = j::Text(j::Object(raw), "id");
        if (hidden_definition(prefix + "definitions/" + id) &&
            std::none_of(cards.begin(), cards.end(), [&](const auto& card) {
              return j::Text(j::Object(card), "id") == id;
            }))
          throw Failure("not_found", "Card not found");
      }
    }
    // A whole-document replacement cannot delete or modify hidden graph items.
    const auto difference = [](const auto& self, const std::string& full,
                               const std::string& visible) -> std::string {
      if (DocumentsEqual(full, visible)) return "null";
      if (full.starts_with('[') && visible.starts_with('[')) {
        auto values = j::Array(full);
        const auto shown = j::Array(visible);
        std::erase_if(values, [&](const auto& value) {
          return std::any_of(
              shown.begin(), shown.end(),
              [&](const auto& other) { return DocumentsEqual(value, other); });
        });
        return j::Encode(values);
      }
      if (full.starts_with('{') && visible.starts_with('{')) {
        auto values = j::Object(full);
        const auto shown = j::Object(visible);
        for (auto it = values.begin(); it != values.end();) {
          it->second = self(self, it->second, j::Get(shown, it->first, "null"));
          if (it->second == "null")
            it = values.erase(it);
          else
            ++it;
        }
        return j::Encode(values);
      }
      return full;
    };
    const auto project_view = [&](const auto& text) {
      return project_document
                 ? ProjectText(text, prefix)
                 : j::Text(
                       j::Object(Filter(j::Encode(j::Members{
                                            {"layout_json", j::Quote(text)}}),
                                        prefix)),
                       "layout_json");
    };
    if (!DocumentsEqual(difference(difference, before, project_view(before)),
                        difference(difference, bytes, project_view(bytes))))
      throw Failure("not_found", "Card not found");
  };
  for (const auto& raw : j::Array(j::Get(plan, "changes", "[]"))) {
    const auto change = j::Object(raw),
               value = j::Object(j::Get(change, "value", "{}"));
    const auto type = j::Text(change, "value_type");
    if (type == "SetProject")
      document("tsugumi.json", j::Text(value, "json"));
    else if (type == "SetLayout")
      document("layout.json", j::Text(value, "json"));
    else if (type == "PutDefinition" || type == "RemoveDefinition") {
      const auto dir = "definitions/" + j::Text(value, "id");
      Check(dir);
      if (type == "RemoveDefinition" && hidden_definition(dir))
        throw Failure("not_found", "Card not found");
      for (const auto* file : {"_card.json", "_meta.json", "_link.json"})
        Check(dir + '/' + file);
      if (type == "PutDefinition")
        document(dir + "/_link.json", j::Text(value, "link_json", "{}"));
    } else if (value.contains("path")) {
      const auto path = j::Text(value, "path");
      Check(path);
      if (type == "RemoveSource" && path.ends_with("/_link.json"))
        document(path, "{}");
      if (type == "PutSource" &&
          (path == "tsugumi.json" || path == "layout.json" ||
           IsExternalDocument(path, "tsugumi.json") ||
           IsExternalDocument(path, "layout.json") ||
           path.ends_with("/_link.json")))
        document(path, Bytes(j::Get(value, "bytes")));
      if (type == "PatchSource" && (IsExternalDocument(path, "tsugumi.json") ||
                                    IsExternalDocument(path, "layout.json") ||
                                    path.ends_with("/_link.json"))) {
        if (!base.files.contains(path))
          throw Failure("not_found", "Path not found");
        const auto& original = base.files.at(path);
        text::PatchApplier apply(original);
        for (const auto& raw_patch : j::Array(j::Get(value, "patches", "[]"))) {
          const auto patch = j::Object(raw_patch);
          const auto offset = j::UInt(
              j::String(j::Get(patch, "offset"), j::Get(patch, "offset")),
              UINT64_MAX);
          const auto remove = j::UInt(
              j::String(j::Get(patch, "remove"), j::Get(patch, "remove")),
              UINT64_MAX);
          if (offset < apply.cursor() || offset > original.size() ||
              remove > original.size() - offset)
            throw Failure("invalid_range", "Invalid patch range");
          apply.append(offset, remove, j::Text(patch, "text"));
        }
        document(path, std::move(apply).finish());
      }
    }
  }
  for (const auto& raw : j::Array(j::Get(plan, "transfers", "[]")))
    Check(j::Text(j::Object(raw), "path"));
}
}  // namespace tsugumi::host
