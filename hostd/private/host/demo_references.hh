#pragma once

#include <filesystem>
#include <string>

namespace tsugumi::host {
class AgentExclude;
struct DemoReferences {
  explicit DemoReferences(std::filesystem::path directory);
  auto List(const AgentExclude* policy = nullptr) -> std::string;
  auto ListFiles() -> std::string;
  auto Read(const std::string& path, const AgentExclude* policy = nullptr)
      -> std::string;
  auto Project(const std::string& name,
               const AgentExclude* policy = nullptr) const
      -> std::filesystem::path;

  std::filesystem::path directory;
};
}  // namespace tsugumi::host
