#include "host/agent_exclude.hh"

#include <algorithm>

#include "host/comments.hh"
#include "host/document_edits.hh"
#include "host/failure.hh"
#include "host/layout_file.hh"
#include "host/layout_saving.hh"
#include "host/save.hh"
#include "tsugumi/document/document.hh"
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
                           const std::string& document, const j::Members& files)
    : root_(Resolve(std::filesystem::absolute(root))) {
  for (const auto& entry : Entries(document)) {
    const std::filesystem::path path(entry);
    paths_.push_back(Resolve(path.is_absolute() ? path : root_ / path));
  }
  for (const auto& [path, raw] : files) {
    if (!path.ends_with("/_card.json")) continue;
    const auto external = j::Text(j::Object(raw), "external");
    if (!external.empty()) links_.emplace(path, external);
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
  if (Contains(definition + "/_card.json")) return false;
  if (slash == std::string::npos) return true;
  // The logical path follows the confirmed definition in this revision.
  const auto link = links_.find(definition + "/_card.json");
  if (link == links_.end()) return false;
  const auto& external = link->second;
  const auto nested = prefix + "externals/" + external + '/';
  return !Contains(nested) && Card(id.substr(slash + 1), nested);
}
auto AgentExclude::CheckRequest(const std::string& name, const j::Members& args,
                                bool recorded) const -> void {
  if (name.starts_with("project_file_")) Check(j::Text(args, "path"));
  if (name.starts_with("version_")) Check(".git");
  if (name.starts_with("run_") || name.starts_with("artifact_") ||
      name == "log_range") {
    Check(".tsugumi/runs.sqlite");
    if (args.contains("run_id"))
      Check(".tsugumi/runs/" + j::Text(args, "run_id"));
  }
  if (recorded) return;
  const auto check_card = [&](const j::Members& card) {
    std::string id;
    for (const auto& segment : j::Array(j::Get(card, "segments", "[]"))) {
      if (!id.empty()) id += '/';
      id += j::String(segment);
    }
    if (!id.empty() && !Card(id)) throw Failure("not_found", "Card not found");
  };
  if (args.contains("segments")) check_card(args);
  for (const auto* key : {"card", "source_card", "trigger", "target"})
    if (args.contains(key) && args.at(key).starts_with('{'))
      check_card(j::Object(args.at(key)));
}
auto AgentExclude::CheckArtifact(const j::Members& artifact,
                                 std::string_view member) const -> void {
  const auto run = j::Text(artifact, "run_id");
  Check(".tsugumi/runs/" + run);
  auto path = std::filesystem::path(".tsugumi/artifacts") / run;
  const auto card = j::Object(j::Get(artifact, "card", "{}"));
  for (const auto& segment : j::Array(j::Get(card, "segments", "[]")))
    path /= j::String(segment);
  const auto label = j::Text(artifact, "label");
  const auto dot = label.find('.');
  if (dot == label.npos) throw Failure("not_found", "Artifact not found");
  const auto invocation = j::Get(artifact, "invocation");
  path /= "@" + j::String(invocation, invocation);
  path /= label.substr(dot + 1);
  Check(path);
  if (!member.empty()) Check(path / member);
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
  return j::Encode(FilterProjectGraph(
      j::Object(raw), [&](const auto& id) { return Card(id, prefix); }));
}
auto AgentExclude::FileText(const std::string& path,
                            const std::string& raw) const -> std::string {
  if (path == "tsugumi.json" || IsExternalDocument(path, "tsugumi.json"))
    return ProjectText(raw, path.substr(0, path.size() - 12));
  if (path == "layout.bin" || IsExternalDocument(path, "layout.bin"))
    return LayoutText(raw, path.substr(0, path.size() - 10));
  return raw;
}

auto AgentExclude::LayoutText(const std::string& view,
                              const std::string& prefix) const -> std::string {
  auto layout = j::Object(view);
  for (const auto* field : {"cards", "boundaries"})
    if (layout.contains(field)) {
      auto entries = j::Object(layout.at(field));
      std::erase_if(entries,
                    [&](const auto& entry) { return !Card(entry.first, prefix); });
      layout[field] = j::Encode(entries);
    }
  return j::Encode(layout);
}

auto AgentExclude::Filter(const std::string& raw, const std::string& prefix,
                          bool recorded) const -> std::string {
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
          CheckRequest("", object, recorded);
        } catch (const Failure&) {
          continue;
        }
      }
      values.push_back(Filter(value, prefix, recorded));
    }
    return j::Encode(values);
  }
  if (!raw.starts_with('{')) return raw;
  auto object = j::Object(raw);
  // A snapshot's or delta's layout is shown as its JSON view, with the text
  // of the notes it carries.
  if (object.contains("layout") && (object.contains("sources") ||
                                    object.contains("sources_upsert"))) {
    std::map<std::string, std::string> notes;
    for (const auto* list : {"sources", "sources_upsert"})
      for (const auto& entry : j::Array(j::Get(object, list, "[]"))) {
        const auto source = j::Object(entry);
        const auto path = j::Text(source, "path");
        if (const auto id = NoteId("", path);
            id && source.contains("document") && !Contains(path))
          notes[*id] = DecodeSourceBytes(source.at("document"));
      }
    try {
      object["layout"] = j::Quote(LayoutText(
          LayoutView(DecodeSourceBytes(object.at("layout")), notes), prefix));
    } catch (const std::exception&) {
      object["layout"] = j::Quote("{}");
    }
  }
  object.erase("document");
  if (object.contains("id") && object.contains("card_json")) {
    const auto dir = prefix + "definitions/" + j::Text(object, "id") + '/';
    if (Contains(dir + "README.md")) object.erase("readme");
    if (Contains(dir + "_comments.json")) object.erase("comments_json");
  }
  for (auto& [key, value] : object) {
    if (key == "project_json")
      value = j::Quote(ProjectText(j::String(value), prefix));
    else if (key != "bytes" && key != "text" && key != "record_json" &&
             key != "card_json" && key != "readme" && key != "comments_json" &&
             key != "layout" && key != "stdout_bytes" && key != "stderr_bytes")
      value = Filter(value, prefix, recorded);
  }
  return j::Encode(object);
}
auto AgentExclude::CheckSave(const j::Members& plan, const SaveBase& base,
                             const SourcePatches& patches,
                             bool allow_template_writes) const -> void {
  auto check_source = [&](const std::string& path) {
    Check(path);
    if (allow_template_writes) return;
    auto local = std::string_view(path);
    while (local.starts_with("externals/")) {
      const auto end = local.find('/', 10);
      if (end == std::string_view::npos) return;
      local.remove_prefix(end + 1);
    }
    if (local.starts_with("templates/"))
      throw Failure("permission_denied",
                    "Use an approved template store operation");
  };
  const auto hidden_tree = [&](const std::string& path) {
    const auto resolved = Resolve(root_ / path);
    return Contains(path) ||
           std::any_of(paths_.begin(), paths_.end(), [&](const auto& entry) {
             return Under(resolved, Resolve(entry));
           });
  };
  const auto hidden_definition = [&](const std::string& dir) {
    if (hidden_tree(dir)) return true;
    const auto link = base.files.find(dir + "/_card.json");
    if (link == base.files.end()) return false;
    const auto external = j::Text(j::Object(link->second), "external");
    const auto prefix = dir.substr(0, dir.rfind("definitions/"));
    return !external.empty() && hidden_tree(prefix + "externals/" + external);
  };
  const auto card_document = [](const std::string& path) {
    return path.ends_with("/_card.json") &&
           comments::is_comments(path.substr(0, path.size() - 10) +
                                 "_comments.json");
  };
  auto document = [&](const std::string& path, const std::string& bytes) {
    Check(path);
    const auto before = base.files.contains(path) ? base.files.at(path) : "{}";
    if (card_document(path)) {
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
        path == "layout.bin" || IsExternalDocument(path, "layout.bin");
    if (!project_document && !layout_document) return;
    const auto prefix =
        path.substr(0, path.size() - (project_document ? 12 : 10));
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
    if (!DocumentsEqual(difference(difference, before, FileText(path, before)),
                        difference(difference, bytes, FileText(path, bytes))))
      throw Failure("not_found", "Card not found");
  };
  for (const auto& raw : j::Array(j::Get(plan, "changes", "[]"))) {
    const auto change = j::Object(raw),
               value = j::Object(j::Get(change, "value", "{}"));
    const auto type = j::Text(change, "value_type");
    if (type == "SetProject")
      document("tsugumi.json", j::Text(value, "json"));
    else if (type == "SetLayout")
      document("layout.bin",
               LayoutView(DecodeSourceBytes(j::Get(value, "layout", "\"\"")),
                          {}));
    else if (type == "PutDefinition" || type == "RemoveDefinition") {
      const auto dir = "definitions/" + j::Text(value, "id");
      Check(dir);
      if (type == "RemoveDefinition" && hidden_definition(dir))
        throw Failure("not_found", "Card not found");
      for (const auto* file : {"_card.json", "README.md", "_comments.json"})
        Check(dir + '/' + file);
      if (type == "PutDefinition")
        document(dir + "/_card.json", j::Text(value, "card_json", "{}"));
    } else if (value.contains("path")) {
      const auto path = j::Text(value, "path");
      check_source(path);
      if (type == "RemoveSource" && card_document(path)) document(path, "{}");
      if (type == "PutSource" && IsExternalDocument(path, "layout.bin"))
        document(path,
                 LayoutView(DecodeSourceBytes(j::Get(value, "bytes")), {}));
      if (type == "PutSource" &&
          (path == "tsugumi.json" || IsExternalDocument(path, "tsugumi.json") ||
           card_document(path)))
        document(path, DecodeSourceBytes(j::Get(value, "bytes")));
      if (type == "PatchSource" &&
          (path == "tsugumi.json" || IsExternalDocument(path, "tsugumi.json") ||
           card_document(path))) {
        if (!base.files.contains(path))
          throw Failure("not_found", "Path not found");
        try {
          document(path,
                   patches.Project(base.files.at(path),
                                   j::Array(j::Get(value, "patches", "[]"))));
        } catch (const text::PatchRangeError& error) {
          throw Failure("invalid_range", error.what());
        }
      }
    }
  }
  for (const auto& raw : j::Array(j::Get(plan, "transfers", "[]")))
    check_source(j::Text(j::Object(raw), "path"));
}
}  // namespace tsugumi::host
