#include "agent/thought_schedule.hh"

#include <stdexcept>

// A socket-free check with a controlled clock: extra tokens are never spent
// for an idle turn, a stream's tool-input fragments, or unchanged work.
auto main() -> int {
  using namespace std::chrono_literals;
  using tsugumi::agent::ThoughtSchedule;
  const auto now = ThoughtSchedule::Clock::time_point{};
  auto require = [](bool value) {
    if (!value) throw std::runtime_error("Thought scheduling regression");
  };
  ThoughtSchedule schedule;
  schedule.Note("text");
  require(!schedule.Begin(false, now));
  schedule.TurnStarted();
  require(!schedule.Begin(true, now));
  schedule.Note("toolInput");
  require(!schedule.Begin(true, now));
  schedule.Note("text");
  require(schedule.Begin(true, now));
  schedule.Note("toolFinished");
  require(!schedule.Begin(true, now + 5s));  // Still in flight.
  schedule.End();
  require(!schedule.Begin(true, now + 3999ms));
  require(schedule.Begin(true, now + 4s));  // Pending work survives the wait.
  schedule.End();
  require(!schedule.Begin(true, now + 8s));
  schedule.Note("toolStarted");
  require(!schedule.Begin(false, now + 8s));
  schedule.TurnStarted();  // Old work cannot trigger a new turn's summary.
  require(!schedule.Begin(true, now + 8s));
  schedule.Note("text");
  require(schedule.Begin(true, now + 8s));
  schedule.End();
  ThoughtSchedule other;
  other.Note("text");
  require(other.Begin(true, now));  // Conversations have independent budgets.
}
