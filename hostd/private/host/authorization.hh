#pragma once

#include <string>
#include <utility>

namespace tsugumi::host {

// Only Access can issue this capability, after a human approves the turn.
// The helper's Card scope still limits which tools and paths it can use.
class TurnAuthorization {
 public:
  auto project() const -> const std::string& { return project_; }
  auto Matches(const std::string& project,
               const std::string& conversation) const -> bool {
    return project_ == project && conversation_ == conversation;
  }

 private:
  friend struct Access;
  TurnAuthorization(std::string project, std::string conversation)
      : project_(std::move(project)), conversation_(std::move(conversation)) {}
  std::string project_;
  std::string conversation_;
};

}  // namespace tsugumi::host
