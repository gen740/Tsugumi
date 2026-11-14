#pragma once

#include <charconv>
#include <cstdint>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "tsugumi/workerd/invocation.hh"
#include "tsugumi/protocol/tsugumi_enums_generated.h"

namespace tsugumi::workerd {

inline constexpr uint64_t kDefaultLogLimitBytes = uint64_t{8} << 20;

inline auto parse_log_limit_bytes(std::string_view value) -> uint64_t {
  uint64_t limit = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), limit);
  if (error != std::errc{} || end != value.data() + value.size() || !limit)
    throw std::runtime_error("invalid log-limit-bytes: expected a positive byte count");
  return limit;
}

// The limit counts complete JSONL entries, including their trailing newlines.
class InvocationLog {
 public:
  InvocationLog(std::ostream& output, uint64_t log_limit_bytes)
      : output_(output), log_limit_bytes_(log_limit_bytes) {}

  auto append(uint64_t stream, const std::string& text) -> void {
    if (text.size() > log_limit_bytes_ - logged_) {
      truncated_ = true;
      return;
    }
    remote::LogJson entry;
    *entry.stream = stream;
    *entry.text = text;
    const auto encoded = schema::encode(entry);
    if (encoded.size() + 1 > log_limit_bytes_ - logged_) {
      truncated_ = true;
      return;
    }
    output_ << encoded << '\n';
    logged_ += encoded.size() + 1;
  }

  auto finish() -> void {
    if (!truncated_) return;
    // Keep the notice outside the budget so even a one-byte limit is explicit.
    remote::LogJson entry;
    *entry.stream = static_cast<uint64_t>(Tsugumi::Data::LogStream::journal);
    *entry.text = "\n[Log output truncated at the " + std::to_string(log_limit_bytes_) + "-byte limit.]\n";
    output_ << schema::encode(entry) << '\n';
    truncated_ = false;
  }

 private:
  std::ostream& output_;
  uint64_t log_limit_bytes_;
  uint64_t logged_ = 0;
  bool truncated_ = false;
};

}  // namespace tsugumi::workerd
