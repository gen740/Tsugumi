#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "tsugumi/workerd/invocation.hh"
#include "tsugumi/workerd/values.hh"

namespace tsugumi::workerd {

// The limit counts complete entries, including their size prefixes.
class InvocationLog {
 public:
  explicit InvocationLog(uint64_t limit_bytes) : limit_bytes_(limit_bytes) {}

  auto append(LogStream stream, const std::string& text)
      -> std::optional<std::string> {
    if (truncated_) return std::nullopt;
    if (text.size() < limit_bytes_ - logged_) {
      auto encoded = remote::encode_log_entry(stream, text);
      if (encoded.size() <= limit_bytes_ - logged_) {
        logged_ += encoded.size();
        return encoded;
      }
    }
    truncate();
    return std::nullopt;
  }

  // A downstream runner may already have omitted bytes under this policy.
  auto truncate() -> void { truncated_ = true; }
  auto truncated() const -> bool { return truncated_; }

 private:
  uint64_t limit_bytes_;
  uint64_t logged_ = 0;
  bool truncated_ = false;
};

}  // namespace tsugumi::workerd
