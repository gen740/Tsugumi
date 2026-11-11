#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "agent/provider.hh"

namespace tsugumi::agent {

struct CatalogTool {
  ToolSpec spec;
  std::string request_name;
  bool mutating = false;
  bool has_origin = false;
};

class Catalog {
public:
  Catalog();
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
