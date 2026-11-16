#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agent/json.hh"
#include "tsugumi/protocol/host_generated.h"
#include "util/sqlite.hh"

namespace tsugumi::host {
inline constexpr size_t kSkillLimit = 512 * 1024;
// An absent declaration is distinct from a declared empty scope. Invalid or
// duplicate declarations fail closed; the body can never change this contract.
auto SkillScope(std::string_view text)
    -> std::optional<std::vector<std::string>>;
auto ScopedSkillTask(std::string_view text, const std::string &name,
                     const std::vector<std::string> &cards,
                     const std::string &instruction) -> agent::json::Members;
auto SelectScopedSkill(const Tsugumi::Host::SkillsListT &catalog,
                       const std::vector<std::string> &names,
                       const std::string &explicit_name,
                       const std::vector<std::string> &cards,
                       const std::string &instruction)
    -> std::optional<agent::json::Members>;
auto BundledSkill(const std::string &path) -> std::string;
auto ReadSkill(const std::filesystem::path &root,
               const std::filesystem::path &file) -> std::string;
auto SkillNameKey(std::string_view name) -> std::string;
// Built-in names resolve by project > registered folders > built-in.
// Untrusted project entries remain available only for explicit selection.
// Metadata does not constitute a Read grant.
auto ProjectTrusted(sql::Db& db, const std::filesystem::path& path,
                    const std::vector<std::string>& folders = {}) -> bool;
auto ProjectTrust(sql::Db& db, const std::filesystem::path& path,
                  const std::vector<std::string>& folders = {})
    -> Tsugumi::Host::ProjectTrustDecision;
auto TrustedParent(const std::filesystem::path& path,
                   const std::vector<std::string>& folders) -> std::string;
auto SetProjectTrust(sql::Db &db, const std::filesystem::path &path,
                     Tsugumi::Host::ProjectTrustDecision decision) -> void;
auto ListSkills(const std::filesystem::path& project,
                const std::vector<std::string>& folders, bool trusted = false)
    -> Tsugumi::Host::SkillsListT;
} // namespace tsugumi::host
