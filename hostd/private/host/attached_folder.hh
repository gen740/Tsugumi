#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>

#include "tsugumi/util/json_object.hh"

namespace tsugumi::host {

class AgentExclude;
struct FolderReadFailure : std::runtime_error {
  FolderReadFailure(std::string kind, std::string message)
      : std::runtime_error(std::move(message)), code(std::move(kind)) {}
  std::string code;
};

// The caller must authorize this registered root and recheck the grant before
// releasing the result. Opens components without following symbolic links.
auto ReadAttachedFolder(const std::filesystem::path& root,
                        const ::tsugumi::json::Members& arguments,
                        const AgentExclude* policy = nullptr) -> std::string;

}  // namespace tsugumi::host
