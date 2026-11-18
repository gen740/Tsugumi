#pragma once

#include <chrono>
#include <optional>
#include <string_view>

namespace tsugumi::agent {

// The host holds its mutex while using this per-conversation schedule.
// Polling a window never implies permission to spend tokens on unchanged work.
class ThoughtSchedule {
 public:
  using Clock = std::chrono::steady_clock;

  auto TurnStarted() -> void { pending_ = false; }

  auto Note(std::string_view kind) -> void {
    if (kind == "text" || kind == "toolStarted" || kind == "toolFinished")
      pending_ = true;
  }

  auto Begin(bool working, Clock::time_point now = Clock::now()) -> bool {
    if (!working || !pending_ || in_flight_ ||
        (last_call_ && now - *last_call_ < std::chrono::seconds(4)))
      return false;
    pending_ = false;
    in_flight_ = true;
    last_call_ = now;
    return true;
  }

  auto End() -> void { in_flight_ = false; }

 private:
  bool pending_ = false;
  bool in_flight_ = false;
  std::optional<Clock::time_point> last_call_;
};

}  // namespace tsugumi::agent
