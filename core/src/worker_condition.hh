#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

namespace tsugumi::worker_condition {

struct Text {
  std::string_view language;
  std::string_view expression;
};

struct Diagnostic {
  std::string message;
  // Zero-based byte offset in expression; its size denotes end of input.
  // An unknown language is reported at zero.
  std::size_t position;
};

class Condition {
 public:
  virtual ~Condition() = default;
  // Immutable, independent of the input text's lifetime, and safe to share.
  virtual auto matches(std::uint64_t grade) const noexcept -> bool = 0;
};

using CompileResult =
    std::variant<std::unique_ptr<const Condition>, Diagnostic>;

// The factory currently registers only "simple"; callers never parse its text.
// Atoms: x, decimal/0x hexadecimal uint64 literals, and parenthesized
// expressions. No separators, other names, functions, floats, strings, or
// implicit booleans. Precedence, tight to loose: unary ~ !; << >>; &; ^; |;
// comparisons; &&; ||. Comparisons < <= > >= == != take integers and return
// bool; no chaining. Bitwise/shift operators take integers; ! && || take bool;
// the root is bool. Binary operators associate left; unary operators associate
// right. Shifts discard outgoing bits; computed counts >=64 yield zero.
// Literal shift counts >=64 are compile errors; evaluation cannot fail.
// Limits: 4096 bytes and 64 nested parentheses/unary operators (combined).
auto compile(Text text) -> CompileResult;

}  // namespace tsugumi::worker_condition
