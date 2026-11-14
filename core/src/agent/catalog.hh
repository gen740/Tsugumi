#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "agent/json.hh"
#include "agent/provider.hh"

namespace tsugumi::agent {

// Internal project RPC encoding is independent of the model/plugin allow list.
// Callers exposing tools must authorize through Catalog before reaching this.
auto EncodeProjectRequest(std::string_view name, std::string_view arguments,
                          uint64_t id) -> std::string;

struct CatalogTool {
  ToolSpec spec;
  std::string request_name;
  bool mutating = false;
  bool has_origin = false;
  bool approval_required = false;
};

class Catalog {
public:
  Catalog();
  auto shared() const -> json::Members;
  auto tools() const -> const std::vector<CatalogTool> & { return tools_; }
  auto Find(std::string_view name) const -> const CatalogTool *;
  auto Json() const -> std::string;
  auto Request(std::string_view name, std::string_view arguments,
               uint64_t id) const -> std::string;
  auto Reply(std::string_view bytes) const -> std::string;

private:
  std::vector<CatalogTool> tools_;
};

} // namespace tsugumi::agent
