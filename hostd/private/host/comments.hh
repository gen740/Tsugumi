// Agent-authored edits are stamped by the host during save.
#pragma once

#include <functional>

#include "tsugumi/document/comments.hh"

namespace tsugumi::comments {
using ReserveComment = std::function<std::string(
    const std::string&, const std::vector<std::string>&)>;
auto stamped(const std::string& text, const std::string& card,
             const std::vector<Comment>& before,
             const std::vector<std::string>& changed,
             const ReserveComment& reserve) -> std::string;
auto record_json(const std::vector<Change>& changes) -> std::string;
}  // namespace tsugumi::comments
