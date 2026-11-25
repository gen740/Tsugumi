// Exported build plans. All package and view paths are relative to Resources.
#pragma once

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "nix.hh"

namespace tsugumi {

struct PackedBuilds {
  bool fast = false;
  bool debug = false;
  std::filesystem::path resources;
  std::set<std::string> host_paths;
  std::map<std::string, CardBuildPlan> cards;

  static auto read(const std::filesystem::path& manifest,
                   const Resolution& resolution) -> PackedBuilds;
  auto libraries() const -> std::vector<std::string>;
  auto build(const BuildRequest& request) const -> CardBuildPlan;
};

}  // namespace tsugumi
