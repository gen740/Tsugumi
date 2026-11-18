#include "tsugumi/document/worker_condition.hh"

#include <array>
#include <charconv>
#include <optional>
#include <utility>
#include <vector>

namespace tsugumi::worker_condition {
namespace {

constexpr std::size_t kMaxBytes = 4096;
constexpr std::size_t kMaxNesting = 64;

enum class Op {
  kEnd,
  kLiteral,
  kX,
  kOpen,
  kClose,
  kNot,
  kInvert,
  kShiftLeft,
  kShiftRight,
  kAnd,
  kXor,
  kOr,
  kLess,
  kLessEqual,
  kGreater,
  kGreaterEqual,
  kEqual,
  kNotEqual,
  kLogicalAnd,
  kLogicalOr,
};

struct Instruction {
  Op op;
  std::uint64_t value = 0;
};

auto unary(Op op, std::uint64_t value) noexcept -> std::uint64_t {
  return op == Op::kNot ? !value : ~value;
}

auto binary(Op op, std::uint64_t left, std::uint64_t right) noexcept
    -> std::uint64_t {
  switch (op) {
    case Op::kShiftLeft:
      return right >= 64 ? 0 : left << right;
    case Op::kShiftRight:
      return right >= 64 ? 0 : left >> right;
    case Op::kAnd:
      return left & right;
    case Op::kXor:
      return left ^ right;
    case Op::kOr:
      return left | right;
    case Op::kLess:
      return left < right;
    case Op::kLessEqual:
      return left <= right;
    case Op::kGreater:
      return left > right;
    case Op::kGreaterEqual:
      return left >= right;
    case Op::kEqual:
      return left == right;
    case Op::kNotEqual:
      return left != right;
    case Op::kLogicalAnd:
      return left && right;
    case Op::kLogicalOr:
      return left || right;
    default:
      return 0;  // Only typed binary instructions reach evaluation.
  }
}

class SimpleCondition final : public Condition {
 public:
  explicit SimpleCondition(std::vector<Instruction> code)
      : code_(std::move(code)) {}

  auto matches(std::uint64_t grade) const noexcept -> bool override {
    // Each push consumes at least one input byte. Compilation bounds both
    // this stack and the instruction count; evaluation allocates and recurses
    // nowhere, including for long left-associative operator chains.
    std::array<std::uint64_t, kMaxBytes> stack;
    std::size_t size = 0;
    for (const auto& instruction : code_) {
      if (instruction.op == Op::kLiteral || instruction.op == Op::kX) {
        stack[size++] = instruction.op == Op::kX ? grade : instruction.value;
      } else if (instruction.op == Op::kNot || instruction.op == Op::kInvert) {
        stack[size - 1] = unary(instruction.op, stack[size - 1]);
      } else {
        const auto right = stack[--size];
        stack[size - 1] = binary(instruction.op, stack[size - 1], right);
      }
    }
    return stack[0] != 0;
  }

 private:
  std::vector<Instruction> code_;
};

struct Token {
  Op op;
  std::size_t position;
  std::uint64_t value = 0;
};

auto precedence(Op op) -> int {
  switch (op) {
    case Op::kLogicalOr:
      return 1;
    case Op::kLogicalAnd:
      return 2;
    case Op::kLess:
    case Op::kLessEqual:
    case Op::kGreater:
    case Op::kGreaterEqual:
    case Op::kEqual:
    case Op::kNotEqual:
      return 3;
    case Op::kOr:
      return 4;
    case Op::kXor:
      return 5;
    case Op::kAnd:
      return 6;
    case Op::kShiftLeft:
    case Op::kShiftRight:
      return 7;
    default:
      return 0;
  }
}

auto ascii_alphanumeric(char c) -> bool {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
         (c >= 'A' && c <= 'Z') || c == '_';
}

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) { advance(); }

  auto parse() -> std::unique_ptr<const Condition> {
    const auto result = expression(1, 0);
    if (token_.op != Op::kEnd) fail(Code::UnexpectedToken, "Unexpected token", token_.position);
    if (!result.boolean) fail(Code::BooleanRequired, "Condition must be boolean", result.position);
    return std::make_unique<SimpleCondition>(std::move(code_));
  }

 private:
  struct Value {
    bool boolean;
    std::optional<std::uint64_t> literal;
    std::size_t position;
  };

  [[noreturn]] static auto fail(Code code, std::string message, std::size_t position)
      -> void {
    throw Diagnostic{code, std::move(message), position};
  }

  auto advance() -> void {
    while (offset_ < text_.size()) {
      const auto c = text_[offset_];
      if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '\f' &&
          c != '\v')
        break;
      ++offset_;
    }
    const auto start = offset_;
    if (start == text_.size()) {
      token_ = {Op::kEnd, start};
      return;
    }
    const auto c = text_[offset_++];
    if (c >= '0' && c <= '9') {
      while (offset_ < text_.size() && ascii_alphanumeric(text_[offset_]))
        ++offset_;
      auto digits = text_.substr(start, offset_ - start);
      const int base = digits.starts_with("0x") ? 16 : 10;
      if (base == 16) digits.remove_prefix(2);
      std::uint64_t number = 0;
      const auto [end, error] = std::from_chars(
          digits.data(), digits.data() + digits.size(), number, base);
      if (error == std::errc::result_out_of_range)
        fail(Code::IntegerOverflow, "Integer literal exceeds uint64", start);
      if (error != std::errc{} || end != digits.data() + digits.size())
        fail(Code::InvalidInteger, "Invalid integer literal", start);
      token_ = {Op::kLiteral, start, number};
      return;
    }
    if (c == 'x' &&
        (offset_ == text_.size() || !ascii_alphanumeric(text_[offset_]))) {
      token_ = {Op::kX, start};
      return;
    }
    struct Spelling {
      std::string_view text;
      Op op;
    };
    static constexpr auto spellings = std::to_array<Spelling>({
        {"<<", Op::kShiftLeft},
        {">>", Op::kShiftRight},
        {"<=", Op::kLessEqual},
        {">=", Op::kGreaterEqual},
        {"==", Op::kEqual},
        {"!=", Op::kNotEqual},
        {"&&", Op::kLogicalAnd},
        {"||", Op::kLogicalOr},
        {"(", Op::kOpen},
        {")", Op::kClose},
        {"!", Op::kNot},
        {"~", Op::kInvert},
        {"&", Op::kAnd},
        {"^", Op::kXor},
        {"|", Op::kOr},
        {"<", Op::kLess},
        {">", Op::kGreater},
    });
    for (const auto& spelling : spellings) {
      if (text_.substr(start).starts_with(spelling.text)) {
        offset_ = start + spelling.text.size();
        token_ = {spelling.op, start};
        return;
      }
    }
    fail(Code::UnexpectedCharacter, "Unexpected character", start);
  }

  auto prefix(std::size_t nesting) -> Value {
    const auto token = token_;
    if (token.op == Op::kLiteral || token.op == Op::kX) {
      code_.push_back({token.op, token.value});
      advance();
      return {
          false,
          token.op == Op::kLiteral ? std::optional(token.value) : std::nullopt,
          token.position};
    }
    if (token.op != Op::kOpen && token.op != Op::kNot &&
        token.op != Op::kInvert)
      fail(Code::ExpressionExpected, "Expected expression", token.position);
    if (nesting == kMaxNesting) fail(Code::NestingLimit, "Nesting exceeds 64", token.position);
    advance();
    if (token.op == Op::kOpen) {
      auto value = expression(1, nesting + 1);
      if (token_.op != Op::kClose) fail(Code::CloseExpected, "Expected ')'", token_.position);
      advance();
      return value;
    }
    auto value = prefix(nesting + 1);
    if (value.boolean != (token.op == Op::kNot))
      fail(token.op == Op::kNot ? Code::BooleanOperand : Code::IntegerOperand,
           token.op == Op::kNot ? "Expected boolean operand"
                                : "Expected integer operand",
           token.position);
    code_.push_back({token.op});
    value.literal.reset();
    value.position = token.position;
    return value;
  }

  auto expression(int minimum, std::size_t nesting) -> Value {
    auto left = prefix(nesting);
    while (precedence(token_.op) >= minimum) {
      const auto token = token_;
      const int priority = precedence(token.op);
      advance();
      const auto right = expression(priority + 1, nesting);
      const bool logical = priority <= 2;
      if (left.boolean != logical || right.boolean != logical)
        fail(logical ? Code::BooleanOperand : Code::IntegerOperand,
            logical ? "Expected boolean operands" : "Expected integer operands",
            token.position);
      if ((token.op == Op::kShiftLeft || token.op == Op::kShiftRight) &&
          right.literal && *right.literal >= 64)
        fail(Code::ShiftLimit, "Literal shift count must be less than 64", right.position);
      code_.push_back({token.op});
      left.boolean = priority <= 3;
      left.literal.reset();
    }
    return left;
  }

  std::string_view text_;
  std::size_t offset_ = 0;
  Token token_{Op::kEnd, 0};
  std::vector<Instruction> code_;
};

auto compile_simple(std::string_view expression) -> CompileResult {
  if (expression.size() > kMaxBytes)
    return Diagnostic{Code::SizeLimit, "Condition exceeds 4096 bytes", kMaxBytes};
  try {
    return Parser(expression).parse();
  } catch (Diagnostic& diagnostic) {
    return std::move(diagnostic);
  }
}

}  // namespace

auto compile(Text text) -> CompileResult {
  // Add another compiler here; neither its program representation nor its
  // grammar is part of the caller's contract.
  struct Language {
    std::string_view name;
    auto (*compile)(std::string_view) -> CompileResult;
  };
  static constexpr std::array languages{Language{"simple", compile_simple}};
  for (const auto& language : languages) {
    if (language.name == text.language)
      return language.compile(text.expression);
  }
  return Diagnostic{Code::UnknownLanguage, "Unknown condition language", 0};
}

}  // namespace tsugumi::worker_condition
