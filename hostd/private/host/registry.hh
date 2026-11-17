#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "host/state_gate.hh"
#include "tsugumi/protocol/host_generated.h"
#include "tsugumi/util/sqlite.hh"

namespace tsugumi::host {
// Registration is serialized by the caller's shared StateGate. The callback
// checks the identity of a conversation database that is already open.
// A canonical folder has one current project identity. Registration replaces
// older IDs for that path; reads reject identities that no longer match disk.
struct ProjectRegistry {
  struct Project {
    std::string endpoint;
    std::string token;
  };
  using ConversationPath =
      std::function<std::optional<std::filesystem::path>(const std::string&)>;
  ProjectRegistry(StateGate& gate, sql::Db& db,
                  ConversationPath conversation_path);
  auto ReplacePath(const std::string& project,
                   const std::filesystem::path& folder) -> void;
  auto Lookup(const std::string& id) -> Project;
  auto Location(const std::string& project)
      -> std::optional<std::filesystem::path>;
  auto Locations() -> std::vector<std::string>;
  auto Root(const std::string& project) -> std::filesystem::path;
  auto ValidateLocation(const std::string& project,
                        const std::filesystem::path& folder) -> void;
  auto RegisterProject(const Tsugumi::Host::RegisterProjectRequest& request)
      -> void;
  auto RegisterFolder(const Tsugumi::Host::RegisterFolderRequest& request)
      -> void;
  auto AttachedRoot(const std::string& project, const std::string& folder) -> std::string;
  auto RegisterReady(const Tsugumi::Host::EnvironmentStateT& state) -> void;
  auto UnregisterEnvironment(const std::string& key) -> void;

  StateGate& gate;
  sql::Db& db;
  ConversationPath conversation_path;
};
}  // namespace tsugumi::host
