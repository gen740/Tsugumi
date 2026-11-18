#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Tsugumi::Host {
struct LayoutSceneT;
struct ProjectMoveT;
} // namespace Tsugumi::Host

namespace tsugumi::host {
struct LayoutFile {
  std::string text;
  std::optional<std::string> hash;
};
// Empty id asks whether the layout file may be written; otherwise checks a
// Card.
using LayoutWrite =
    std::function<bool(const std::string &, const std::string &)>;
using LayoutRead =
    std::function<std::optional<LayoutFile>(const std::string &)>;
// Only external project documents, never a Card's source file of that name.
auto IsExternalDocument(std::string_view path, std::string_view name) -> bool;
// Returns the layout with places for every Card in this workflow. Existing
// entries (including a person's drop) are preserved byte-for-byte as values.
auto CompleteLayout(const std::string& project, const std::string& layout,
                    const LayoutRead& read = {}) -> std::string;
// Explicit Agent operation, using the same host geometry as missing placement.
auto ArrangeProjectLayout(
    const std::string &project, const std::string &layout,
                          const LayoutRead& read, bool untangle,
    const Tsugumi::Host::LayoutSceneT *measurements = nullptr) -> std::string;
// Move a selection and reorder its neighbouring ports in the same save.
auto MoveProjectLayout(const std::string &project, const std::string &layout,
                       const LayoutRead &read,
                       const Tsugumi::Host::ProjectMoveT &move) -> std::string;
// Every saving client includes missing places and deletion cleanup in its
// single revision/hash-guarded transaction. Reads use that revision.
auto PrepareLayoutSave(
    const std::string& arguments, const LayoutRead& read,
    const std::vector<std::string>& documents = {"tsugumi.json"},
    const LayoutWrite& may_write = {}) -> std::string;
}  // namespace tsugumi::host
