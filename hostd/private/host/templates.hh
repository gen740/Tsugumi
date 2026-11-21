#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <stop_token>
#include <string>
#include <vector>

#include "tsugumi/protocol/host_generated.h"

namespace tsugumi::host {
// Bound the payload so it and the save metadata fit the core RPC frame.
inline constexpr size_t kTemplateBytes = 4 << 20;
inline constexpr size_t kTemplateEntries = 4096;
using TemplateFiles = std::map<std::string, std::string>;
using TemplatePathCheck = std::function<void(const std::string&)>;

struct DocumentTemplate {
  TemplateFiles files;
  std::vector<std::string> cards;
};

auto BuiltinCard(Tsugumi::Host::BuiltinCard kind, const std::string& id)
    -> TemplateFiles;
auto TemplateName(const std::string& name) -> bool;
auto CheckTemplate(TemplateFiles files) -> DocumentTemplate;
auto ReadTemplateSource(const Tsugumi::Host::TemplateAddT& request,
                        std::stop_token stop) -> DocumentTemplate;
auto ReadHostTemplate(const std::filesystem::path& store,
                      const std::string& name, const TemplatePathCheck& check)
    -> DocumentTemplate;
auto HostTemplateNames(const std::filesystem::path& store)
    -> std::vector<std::string>;
auto AddHostTemplate(const std::filesystem::path& store,
                     const std::string& name, const DocumentTemplate& contents)
    -> void;
}  // namespace tsugumi::host
