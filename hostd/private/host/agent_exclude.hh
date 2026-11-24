#pragma once

#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "host/save.hh"

namespace tsugumi::host {
// Agent policy belongs to the host. Paths always refer to original content,
// even when a shell reads a staged copy or a mounted attachment.
class AgentExclude {
 public:
  static auto Defaults() -> std::vector<std::string>;
  static auto Entries(const std::string& project_json)
      -> std::vector<std::string>;
  AgentExclude(std::filesystem::path root, const std::string& project_json,
               const ::tsugumi::json::Members& files = {});
  auto Contains(const std::filesystem::path& path) const -> bool;
  auto Check(const std::filesystem::path& path) const -> void;
  auto CheckRequest(const std::string& name,
                    const ::tsugumi::json::Members& args,
                    bool recorded = false) const -> void;
  auto CheckContent(const std::string& raw) const -> void;
  auto CheckSave(const ::tsugumi::json::Members& plan, const SaveBase& base,
                 const SourcePatches& patches = {},
                 bool allow_template_writes = false) const -> void;
  auto Filter(const std::string& raw, const std::string& prefix = "",
              bool recorded = false) const -> std::string;
  auto ProjectText(const std::string& raw, const std::string& prefix = "") const
      -> std::string;
  auto FileText(const std::string& path, const std::string& raw) const
      -> std::string;
  // A layout JSON view without the places of hidden Cards.
  auto LayoutText(const std::string& view, const std::string& prefix = "") const
      -> std::string;
  auto Paths() const -> const std::vector<std::filesystem::path>& {
    return paths_;
  }

 private:
  auto Card(const std::string& id, const std::string& prefix = "") const
      -> bool;
  std::filesystem::path root_;
  std::vector<std::filesystem::path> paths_;
  std::set<std::string> subgraphs_;
};
}  // namespace tsugumi::host
