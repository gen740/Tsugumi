#include "agent/thought_schedule.hh"

#include <gtest/gtest.h>

namespace tsugumi::agent {
namespace {

// A socket-free check with a controlled clock: extra tokens are never spent
// for an idle turn, a stream's tool-input fragments, or unchanged work.
TEST(ThoughtScheduleTest, OnlyChangedWorkInTheCurrentTurnSpendsTokens) {
  using namespace std::chrono_literals;
  const auto now = ThoughtSchedule::Clock::time_point{};
  ThoughtSchedule schedule;
  schedule.Note("text");
  EXPECT_FALSE(schedule.Begin(false, now));
  schedule.TurnStarted();
  EXPECT_FALSE(schedule.Begin(true, now));
  schedule.Note("toolInput");
  EXPECT_FALSE(schedule.Begin(true, now));
  schedule.Note("text");
  EXPECT_TRUE(schedule.Begin(true, now));
  schedule.Note("toolFinished");
  EXPECT_FALSE(schedule.Begin(true, now + 5s));  // Still in flight.
  schedule.End();
  EXPECT_FALSE(schedule.Begin(true, now + 3999ms));
  EXPECT_TRUE(schedule.Begin(true, now + 4s));  // Pending work survives the wait.
  schedule.End();
  EXPECT_FALSE(schedule.Begin(true, now + 8s));
  schedule.Note("toolStarted");
  EXPECT_FALSE(schedule.Begin(false, now + 8s));
  schedule.TurnStarted();  // Old work cannot trigger a new turn's summary.
  EXPECT_FALSE(schedule.Begin(true, now + 8s));
  schedule.Note("text");
  EXPECT_TRUE(schedule.Begin(true, now + 8s));
  schedule.End();
  ThoughtSchedule other;
  other.Note("text");
  EXPECT_TRUE(other.Begin(true, now));  // Conversations have independent budgets.
}

}  // namespace
}  // namespace tsugumi::agent
