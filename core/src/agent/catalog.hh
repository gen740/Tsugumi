#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "util/json_object.hh"
#include "agent/provider.hh"

namespace tsugumi::agent {

// Internal project RPC encoding is independent of the model/plugin allow list.
// Callers exposing tools must authorize through Catalog before reaching this.
auto EncodeProjectRequest(std::string_view name, std::string_view arguments,
                          uint64_t id) -> std::string;

struct CatalogTool {
  ToolSpec spec;
  std::string request_name;
  bool host = false;
  bool mutating = false;
  bool has_origin = false;
  bool approval_required = false;
};

enum class ModelRole { Main, Helper, ScopedHelper, Guide };

class Catalog {
public:
  Catalog();
  auto shared() const -> json::Members;
  auto tools() const -> const std::vector<CatalogTool> & { return tools_; }
  auto Find(std::string_view name) const -> const CatalogTool *;
  auto Json() const -> std::string;
  auto AllSpecs() const -> std::vector<ToolSpec>;
  auto ModelSpecs(ModelRole role = ModelRole::Main,
                  bool can_delegate = true) const -> std::vector<ToolSpec>;
  auto FamilySpecs(std::string_view family, ModelRole role) const
      -> std::vector<ToolSpec>;
  auto FamilyTool(std::string_view family, std::string_view action,
                  ModelRole role) const -> std::string;
  auto Allows(std::string_view name, ModelRole role) const -> bool;
  auto ValidateArguments(std::string_view name,
                         std::string_view arguments) const -> void;
  auto Request(std::string_view name, std::string_view arguments,
               uint64_t id) const -> std::string;
  auto Reply(std::string_view bytes) const -> std::string;
  auto HostRequest(std::string_view name, std::string_view arguments,
                   uint64_t id) const -> std::string;
  auto HostReply(std::string_view bytes) const -> std::string;

private:
  std::vector<CatalogTool> tools_;
};

} // namespace tsugumi::agent
