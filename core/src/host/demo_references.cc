#include "host/demo_references.hh"

#include <utility>
#include <vector>

#include "agent/json.hh"
#include "host/failure.hh"
#include "util/fs.hh"

namespace tsugumi::host {
namespace json = agent::json;
DemoReferences::DemoReferences(std::filesystem::path root)
    : directory(std::move(root)) {}

auto DemoReferences::List() -> std::string {
  if (directory.empty()) return "[]";
  std::vector<std::string> files;
  for (const auto& path : fs::list_beneath(directory, ".").files)
    files.push_back(json::Quote(path));
  return json::Encode(files);
}
auto DemoReferences::Read(const std::string& path) -> std::string {
  if (directory.empty() || !fs::valid_relative_path(path))
    throw Failure("not_found", "Demo reference not found");
  auto value = fs::read_beneath(directory, path);
  if (value.size() > (1u << 20))
    throw Failure("limit_exceeded", "Demo reference is too large");
  return value;
}
}  // namespace tsugumi::host
