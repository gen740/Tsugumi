#include "host/demo_references.hh"

#include <algorithm>
#include <utility>
#include <vector>

#include "tsugumi/util/json_object.hh"
#include "host/failure.hh"
#include "host/agent_exclude.hh"
#include "tsugumi/util/fs.hh"

namespace tsugumi::host {
namespace json = ::tsugumi::json;
DemoReferences::DemoReferences(std::filesystem::path root)
    : directory(std::move(root)) {}

auto DemoReferences::List(const AgentExclude* policy) -> std::string {
  if (directory.empty())
    return "[]";
  std::vector<std::string> demos;
  for (const auto *language : {"ja", "en"}) {
    if (policy && policy->Contains(directory / language / "index.json")) continue;
    const auto index = fs::read_file(directory / language / "index.json");
    if (!index)
      continue;
    for (const auto &raw :
         json::Array(json::Get(json::Object(*index), "demos", "[]"))) {
      const auto demo = json::Object(raw);
      if (policy && policy->Contains(directory / language / json::Text(demo, "path"))) continue;
      demos.push_back(json::Encode(json::Members{
          {"path",
           json::Quote(std::string(language) + "/" + json::Text(demo, "path"))},
          {"name", json::Quote(json::Text(demo, "title"))},
          {"description", json::Quote(json::Text(demo, "description"))}}));
    }
  }
  return json::Encode(demos);
}
auto DemoReferences::ListFiles() -> std::string {
  if (directory.empty())
    return "[]";
  std::vector<std::string> files;
  for (const auto &path : fs::list_beneath(directory, ".").files)
    files.push_back(json::Quote(path));
  return json::Encode(files);
}
auto DemoReferences::Read(const std::string &path, const AgentExclude* policy) -> std::string {
  if (directory.empty() || !fs::valid_relative_path(path))
    throw Failure("not_found", "Demo reference not found");
  if (policy) policy->Check(directory / path);
  auto value = fs::read_beneath(directory, path);
  if (value.size() > (1u << 20))
    throw Failure("limit_exceeded", "Demo reference is too large");
  if (policy && (path == "ja/index.json" || path == "en/index.json")) {
    auto index = json::Object(value);
    auto demos = json::Array(json::Get(index, "demos", "[]"));
    std::erase_if(demos, [&](const auto& raw) {
      return policy->Contains(directory / path.substr(0, 2) / json::Text(json::Object(raw), "path"));
    });
    index["demos"] = json::Encode(demos);
    value = json::Encode(index);
  }
  return value;
}
auto DemoReferences::Project(const std::string& name, const AgentExclude* policy) const -> std::filesystem::path {
  // Use the catalog's language-qualified paths, exactly as demo_list does.
  if (!fs::valid_relative_path(name)) throw Failure("not_found", "Demo not found");
  for (const auto* language : {"ja", "en"}) {
    if (policy && policy->Contains(directory / language / "index.json")) continue;
    const auto index = fs::read_file(directory / language / "index.json");
    if (!index) continue;
    for (const auto& raw : json::Array(json::Get(json::Object(*index), "demos", "[]"))) {
      const auto path = std::string(language) + '/' + json::Text(json::Object(raw), "path");
      if (name != path) continue;
      const auto folder = std::filesystem::canonical(directory / path);
      if (!fs::resolves_inside(directory, folder)) break;
      if (policy) policy->Check(folder);
      return folder;
    }
  }
  throw Failure("not_found", "Demo not found");
}
} // namespace tsugumi::host
