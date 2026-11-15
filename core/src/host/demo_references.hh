#pragma once

#include <filesystem>
#include <string>

namespace tsugumi::host {
struct DemoReferences {
  explicit DemoReferences(std::filesystem::path directory);
  auto List() -> std::string;
  auto Read(const std::string& path) -> std::string;

  std::filesystem::path directory;
};
}  // namespace tsugumi::host
