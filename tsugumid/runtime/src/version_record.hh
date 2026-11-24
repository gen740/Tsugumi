// Git commit messages. Card references are permanent ids.
#pragma once

#include <string>

namespace tsugumi::version_record {
inline auto commit(const std::string& kind, const std::string& card = "")
    -> std::string {
  if (kind == "create") return "Initialize Tsugumi project";
  if (kind == "checkout") return "Restore Tsugumi version";
  return "Run Tsugumi card " + card;
}
}  // namespace tsugumi::version_record
