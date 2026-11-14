#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace tsugumi::host {
struct LayoutFile {
  std::string text;
  std::optional<std::string> hash;
};
using LayoutRead = std::function<std::optional<LayoutFile>(const std::string&)>;
// Only external project documents, never a Card's source file of that name.
auto IsExternalDocument(std::string_view path, std::string_view name) -> bool;
// The Agent's saving client validates changed layouts and includes deletion
// cleanup in the same revision/hash-guarded transaction. Reads use that
// revision.
auto PrepareLayoutSave(const std::string& arguments, const LayoutRead& read)
    -> std::string;
}  // namespace tsugumi::host
