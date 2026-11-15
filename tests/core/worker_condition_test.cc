#include "worker_condition.hh"

#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <random>
#include <string>

namespace tsugumi::worker_condition {
namespace {

constexpr auto kMax = std::numeric_limits<std::uint64_t>::max();
struct Example {
  std::string_view expression;
  std::uint64_t grade;
  bool expected;
};
constexpr auto kExamples = std::to_array<Example>({
    {"x > 1", 0, false},
    {"x > 1", 1, false},
    {"x > 1", 2, true},
    {"x & 0x100000000 != 0", 0, false},
    {"x & 0x100000000 != 0", 0x100000000, true},
    {"x & 0x100000000 != 0", 0xffffffff, false},
    {"x & 0x100000000 != 0", kMax, true},
    {"x & 4 != 0", 4, true},
    {"x & 4 != 0", 3, false},
    {"x < 1", 0, true},
    {"x < 1", 1, false},
    {"x <= 1", 1, true},
    {"x <= 1", 2, false},
    {"x >= 1", 1, true},
    {"x >= 1", 0, false},
    {"x == 1", 1, true},
    {"x == 1", 0, false},
    {"x != 1", 1, false},
    {"x != 1", 0, true},
    {"x | 4 == 5", 1, true},
    {"x | 4 == 5", 2, false},
    {"x ^ 4 == 5", 1, true},
    {"x ^ 4 == 5", 5, false},
    {"~x == 0", kMax, true},
    {"~x == 0", 0, false},
    {"x << 1 == 2", 1, true},
    {"x << 1 == 2", 2, false},
    {"x >> 1 == 2", 4, true},
    {"x >> 1 == 2", 2, false},
    {"!(x == 0)", 1, true},
    {"!(x == 0)", 0, false},
    {"x > 1 && x < 4", 2, true},
    {"x > 1 && x < 4", 4, false},
    {"x == 0 || x == 4", 4, true},
    {"x == 0 || x == 4", 1, false},
    {"x == 0 || x == 1 && x == 2", 0, true},
    {"(x == 0 || x == 1) && x == 2", 0, false},
    {"x | 2 ^ 3 & 1 == 3", 1, true},
    {"x & 1 << 2 != 0", 4, true},
    {"~x & 1 == 0", 1, true},
    {"!!(x == 1)", 1, true},
    {"~~x == x", kMax, true},
    {"x >> 1 >> 1 == 2", 8, true},
    {"x << 1 << 2 == 8", 1, true},
    {"x << (1 << 2) == 16", 1, true},
    {"x & 3 ^ 2 | 8 == 11", 1, true},
    {"x & 4 < 1", 0, true},
    {"x & 4 <= 4", 4, true},
    {"x | 4 > 3", 0, true},
    {"x ^ 4 >= 4", 0, true},
    {"x & 4 == 4", 4, true},
    {"\t\n x\r &\f 0xFf\v != 0  ", 128, true},
    {"x == 00012", 12, true},
    {"x == 0x000aBc", 2748, true},
    {"x == 18446744073709551615", kMax, true},
    {"x == 0xffffffffffffffff", kMax, true},
    {"x == 0", kMax, false},
    {"x > 0", kMax, true},
    {"x < 18446744073709551615", kMax, false},
    {"~0 == 18446744073709551615", 0, true},
    {"x << 63 == 0x8000000000000000", 1, true},
    {"x >> 63 == 1", kMax, true},
    {"x << 1 == 18446744073709551614", kMax, true},
    {"x << 1 == 0", 0x8000000000000000, true},
    {"x << 0 == x && x >> 0 == x", kMax, true},
    {"1 << x == 0", 64, true},
    {"1 >> x == 0", 64, true},
    {"1 << x == 0", kMax, true},
    {"1 >> x == 0", kMax, true},
    {"1 << x == 0x8000000000000000", 63, true},
    {"0xffffffffffffffff >> x == 1", 63, true},
    {"x << (64 & 63) == x", kMax, true},
    {"x >> ~0 == 0", kMax, true},
    {"x << (32 << 1) == 0", 1, true},
    {"x >> (0xffffffffffffffff ^ 0xffffffffffffffc0) == 1", kMax, true},
    {"0 == 1 && 1 << x == 0 || 1 == 1", kMax, true},
    {"(x < 1) || !(x >= 1)", 0, true},
});

class WorkerConditionValue : public testing::TestWithParam<Example> {};
TEST_P(WorkerConditionValue, Matches) {
  const auto& example = GetParam();
  SCOPED_TRACE(example.expression);
  auto result = compile({"simple", example.expression});
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<const Condition>>(result));
  const auto& condition = std::get<std::unique_ptr<const Condition>>(result);
  ASSERT_NE(condition, nullptr);
  EXPECT_EQ(condition->matches(example.grade), example.expected);
}
INSTANTIATE_TEST_SUITE_P(Table, WorkerConditionValue,
                         testing::ValuesIn(kExamples));

struct ErrorExample {
  std::string_view expression;
  std::size_t position;
  std::string_view message;
};
constexpr auto kErrors = std::to_array<ErrorExample>({
    {"", 0, "Expected expression"},
    {" \t", 2, "Expected expression"},
    {"x & 4", 0, "Condition must be boolean"},
    {"  1", 2, "Condition must be boolean"},
    {"x", 0, "Condition must be boolean"},
    {"~x", 0, "Condition must be boolean"},
    {"x >", 3, "Expected expression"},
    {"x &&", 4, "Expected expression"},
    {"()", 1, "Expected expression"},
    {"(x == 1", 7, "Expected ')'"},
    {"(x == 1 x)", 8, "Expected ')'"},
    {"x == 1)", 6, "Unexpected token"},
    {"x x", 2, "Unexpected token"},
    {"x == 1 2", 7, "Unexpected token"},
    {"x < 2 < 3", 6, "Expected integer operands"},
    {"x == 2 != 3", 7, "Expected integer operands"},
    {"x < 2 == 1", 6, "Expected integer operands"},
    {"(x == 1) == (x == 2)", 9, "Expected integer operands"},
    {"!x", 0, "Expected boolean operand"},
    {"~(x == 0)", 0, "Expected integer operand"},
    {"x && 1", 2, "Expected boolean operands"},
    {"(x == 1) || 0", 9, "Expected boolean operands"},
    {"(x == 1) & 1", 9, "Expected integer operands"},
    {"1 | (x == 1)", 2, "Expected integer operands"},
    {"1 ^ (x == 1)", 2, "Expected integer operands"},
    {"x << (x == 1) == 0", 2, "Expected integer operands"},
    {"(x == 1) >> 1 == 0", 9, "Expected integer operands"},
    {"x << 64 == 0", 5, "Literal shift count must be less than 64"},
    {"x >> 0x40 == 0", 5, "Literal shift count must be less than 64"},
    {"x << (64) == 0", 6, "Literal shift count must be less than 64"},
    {"x >> 18446744073709551615 == 0", 5,
     "Literal shift count must be less than 64"},
    {"x == 18446744073709551616", 5, "Integer literal exceeds uint64"},
    {"x == 0x10000000000000000", 5, "Integer literal exceeds uint64"},
    {"x == 0x", 5, "Invalid integer literal"},
    {"x == 0xg", 5, "Invalid integer literal"},
    {"x == 12a", 5, "Invalid integer literal"},
    {"x == 1_000", 5, "Invalid integer literal"},
    {"x == 0X10", 5, "Invalid integer literal"},
    {"y == 0", 0, "Unexpected character"},
    {"xx == 0", 0, "Unexpected character"},
    {"x0 == 0", 0, "Unexpected character"},
    {"true", 0, "Unexpected character"},
    {"f(x)", 0, "Unexpected character"},
    {"x == -1", 5, "Unexpected character"},
    {"x == +1", 5, "Unexpected character"},
    {"x + 1 == 2", 2, "Unexpected character"},
    {"x == 1.0", 6, "Unexpected character"},
    {"x == \"1\"", 5, "Unexpected character"},
    {"x = 1", 2, "Unexpected character"},
    {"x ** 2", 2, "Unexpected character"},
    {"x == 0;", 6, "Unexpected character"},
    {"x == 0 // comment", 7, "Unexpected character"},
    {"x == 日本語", 5, "Unexpected character"},
});

auto expect_error(std::string_view expression, std::size_t position,
                  std::string_view message) -> void {
  auto result = compile({"simple", expression});
  ASSERT_TRUE(std::holds_alternative<Diagnostic>(result)) << expression;
  const auto& diagnostic = std::get<Diagnostic>(result);
  EXPECT_EQ(diagnostic.position, position) << expression;
  EXPECT_EQ(diagnostic.message, message) << expression;
}

class WorkerConditionError : public testing::TestWithParam<ErrorExample> {};
TEST_P(WorkerConditionError, ReportsPositionAndMessage) {
  const auto& error = GetParam();
  expect_error(error.expression, error.position, error.message);
}
INSTANTIATE_TEST_SUITE_P(Table, WorkerConditionError,
                         testing::ValuesIn(kErrors));

TEST(WorkerCondition, DispatchesByLanguageWithoutFallback) {
  for (const auto language : {"", "Simple", "future-lisp"}) {
    auto result = compile({language, "x == 0"});
    ASSERT_TRUE(std::holds_alternative<Diagnostic>(result));
    EXPECT_EQ(std::get<Diagnostic>(result).position, 0u);
    EXPECT_EQ(std::get<Diagnostic>(result).message,
              "Unknown condition language");
  }
}

TEST(WorkerCondition, OwnsItsProgramAndCanBeReused) {
  auto result = [] {
    std::string language = "simple";
    std::string text = "x & 4 != 0";
    return compile({language, text});
  }();
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<const Condition>>(result));
  auto condition =
      std::move(std::get<std::unique_ptr<const Condition>>(result));
  for (std::uint64_t x = 0; x < 256; ++x)
    EXPECT_EQ(condition->matches(x), (x & 4) != 0);
}

TEST(WorkerCondition, BoundsInputBytesIncludingWhitespaceAndNulls) {
  std::string text = "x == 0";
  text.resize(4096, ' ');
  auto result = compile({"simple", text});
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<const Condition>>(result));
  EXPECT_TRUE(std::get<std::unique_ptr<const Condition>>(result)->matches(0));
  text += ' ';
  expect_error(text, 4096, "Condition exceeds 4096 bytes");
  expect_error(std::string(4097, '0'), 4096, "Condition exceeds 4096 bytes");
  expect_error(std::string("x == 0\0", 7), 6, "Unexpected character");
}

TEST(WorkerCondition, BoundsCombinedParenthesisAndUnaryNesting) {
  const auto accepts = [](const std::string& text) {
    auto result = compile({"simple", text});
    ASSERT_TRUE(
        std::holds_alternative<std::unique_ptr<const Condition>>(result))
        << text;
    EXPECT_TRUE(std::get<std::unique_ptr<const Condition>>(result)->matches(0));
  };
  accepts(std::string(64, '(') + "x == 0" + std::string(64, ')'));
  expect_error(std::string(65, '(') + "x == 0" + std::string(65, ')'), 64,
               "Nesting exceeds 64");
  accepts(std::string(64, '~') + "x == 0");
  expect_error(std::string(65, '~') + "x == 0", 64, "Nesting exceeds 64");
  accepts(std::string(63, '!') + "(x != 0)");
  expect_error(std::string(64, '!') + "(x != 0)", 64, "Nesting exceeds 64");
  accepts(std::string(32, '(') + std::string(32, '~') + "x == 0" +
          std::string(32, ')'));
  expect_error(std::string(32, '(') + std::string(33, '~') + "x == 0" +
                   std::string(32, ')'),
               64, "Nesting exceeds 64");
}

TEST(WorkerCondition, LongFlatChainsDoNotConsumeNestingOrEvaluationStack) {
  std::string text = "x";
  for (int i = 0; i < 2045; ++i) text += "|x";
  text += "==0";
  ASSERT_EQ(text.size(), 4094u);
  auto result = compile({"simple", text});
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<const Condition>>(result));
  const auto& condition = std::get<std::unique_ptr<const Condition>>(result);
  EXPECT_TRUE(condition->matches(0));
  EXPECT_FALSE(condition->matches(kMax));
}

TEST(WorkerCondition, SeededTokenSequencesAndBytesNeverCrash) {
  constexpr std::uint64_t kSeed = 0x75a1;
  std::mt19937_64 random(kSeed);
  constexpr auto tokens = std::to_array<std::string_view>({
      "x",
      "0",
      "1",
      "63",
      "64",
      "18446744073709551615",
      "18446744073709551616",
      "0x100000000",
      "0x",
      "0xffffffffffffffff",
      "<",
      "<=",
      ">",
      ">=",
      "==",
      "!=",
      "&",
      "|",
      "^",
      "~",
      "<<",
      ">>",
      "&&",
      "||",
      "!",
      "(",
      ")",
      " ",
      "\n",
      "y",
      "_",
  });
  int accepted = 0;
  int rejected = 0;
  for (int case_index = 0; case_index < 10000; ++case_index) {
    SCOPED_TRACE(testing::Message()
                 << "seed=" << kSeed << " case=" << case_index);
    std::string text = case_index % 2 == 0 ? "x == 0 " : "";
    const auto count = random() % 64;
    for (std::uint64_t i = 0; i < count; ++i) {
      if (case_index % 4 == 3)
        text += static_cast<char>(random() & 255);
      else
        text += tokens[random() % tokens.size()];
    }
    auto result = compile({"simple", text});
    if (const auto* diagnostic = std::get_if<Diagnostic>(&result)) {
      ++rejected;
      EXPECT_LE(diagnostic->position, text.size());
      EXPECT_FALSE(diagnostic->message.empty());
    } else {
      ++accepted;
      const auto& condition =
          std::get<std::unique_ptr<const Condition>>(result);
      ASSERT_NE(condition, nullptr);
      for (const auto x : {std::uint64_t{0}, std::uint64_t{63},
                           std::uint64_t{64}, kMax, random()}) {
        EXPECT_EQ(condition->matches(x), condition->matches(x));
      }
    }
  }
  EXPECT_GT(accepted, 0);
  EXPECT_GT(rejected, 0);
}

TEST(WorkerCondition, SeededValidProgramsAgreeWithUnsignedOperations) {
  std::mt19937_64 random(0x649a);
  for (int i = 0; i < 1000; ++i) {
    const auto mask = random();
    const auto shift = random() % 64;
    const auto threshold = random();
    const auto text = "(~x & " + std::to_string(mask) + ") >> " +
                      std::to_string(shift) +
                      " >= " + std::to_string(threshold) + " || 1 << x == 0";
    SCOPED_TRACE(text);
    auto result = compile({"simple", text});
    ASSERT_TRUE(
        std::holds_alternative<std::unique_ptr<const Condition>>(result));
    const auto& condition = std::get<std::unique_ptr<const Condition>>(result);
    for (const auto x : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{63},
                         std::uint64_t{64}, kMax, random()}) {
      const bool expected = ((~x & mask) >> shift) >= threshold || x >= 64;
      EXPECT_EQ(condition->matches(x), expected) << x;
    }
  }
}

}  // namespace
}  // namespace tsugumi::worker_condition
