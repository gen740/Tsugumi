#include "host/file_miss.hh"

#include <set>

#include "tsugumi/util/fs.hh"

namespace tsugumi::host {
namespace json = ::tsugumi::json;
auto DescribeFileMiss(const std::string& path, const json::Members& snapshot,
                      const SaveRead& read) -> std::string {
  if (!fs::valid_relative_path(path)) return {};
  size_t start = 0;
  while (path.substr(start).starts_with("externals/")) {
    const auto end = path.find('/', start + 10);
    if (end == std::string::npos) return {};
    start = end + 1;
  }
  if (!path.substr(start).starts_with("definitions/")) return {};
  const auto end = path.find('/', start + 12);
  if (end == std::string::npos) return {};
  const auto id = path.substr(start + 12, end - start - 12);
  const auto root = path.substr(0, end + 1);
  std::set<std::string> files;
  std::string card;
  if (start == 0) {
    for (const auto& raw :
         json::Array(json::Get(snapshot, "definitions", "[]"))) {
      const auto definition = json::Object(raw);
      if (json::Text(definition, "id") != id) continue;
      card = json::Text(definition, "card_json");
      for (const auto& [key, file] : {std::pair{"card_json", "_card.json"},
                                      {"meta_json", "_meta.json"},
                                      {"link_json", "_link.json"}})
        if (!json::Text(definition, key).empty()) files.insert(file);
    }
  }
  for (const auto& raw : json::Array(json::Get(snapshot, "sources", "[]"))) {
    const auto file = json::Text(json::Object(raw), "path");
    if (file.starts_with(root)) files.insert(file.substr(root.size()));
  }
  // A later inspection may already contain the missing file. Do not describe
  // that inspection as evidence of its absence.
  if (files.contains(path.substr(root.size()))) return {};
  if (start != 0 && files.contains("_card.json")) {
    const auto value =
        read("project_file_read", {{"path", json::Quote(root + "_card.json")},
                                   {"at_rev", json::Get(snapshot, "rev")}});
    card = json::Text(value, "bytes");
  }
  auto kind = card.empty() ? (files.contains("_link.json") ? "subgraph" : "")
                           : json::Text(json::Object(card), "template", "code");
  if (!card.empty()) {
    const std::set<std::string> builtins{"trigger", "input",   "branch",
                                         "merge",   "collect", "draft"};
    if (!builtins.contains(kind)) kind = "code";
  }
  if (kind.empty()) return {};
  std::string out = id + " is a " + kind + " Card; it has ";
  constexpr size_t kLimit = 6;
  size_t count = 0;
  for (const auto& file : files) {
    if (count == kLimit) break;
    if (count++) out += ", ";
    out += file;
  }
  if (files.size() > count)
    out += " (" + std::to_string(files.size()) + " files total)";
  const auto file = path.substr(root.size());
  if (file == "_inputs.fbs" || file == "_outputs.fbs") {
    if (kind != "code" && kind != "input") {
      out += "; " + kind + " Cards have no " + file +
             "; project_inspect reports resolved port types in "
             "resolution[].inputs/outputs"
             " (root_type and schema_bfbs)";
      if (kind == "branch" || kind == "merge" || kind == "collect")
        out += "; their types follow the connected upstream ports";
    }
  }
  for (auto& c : out)
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
  return out;
}
}  // namespace tsugumi::host
