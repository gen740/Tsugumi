#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "tsugumi/protocol/host_generated.h"

namespace tsugumi::host {
inline constexpr size_t kSkillLimit = 512 * 1024;
auto ReadSkill(const std::filesystem::path& root,
               const std::filesystem::path& file) -> std::string;
auto SkillNameKey(std::string_view name) -> std::string;
// Swift's catalog order: bundled, project, then enabled folders in settings
// order. Duplicate names are retained. Metadata does not constitute a read
// grant.
auto ListSkills(const std::filesystem::path& project,
                const std::vector<std::string>& folders)
    -> Tsugumi::Host::SkillsListT;
}  // namespace tsugumi::host
