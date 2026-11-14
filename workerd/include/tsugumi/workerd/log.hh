#pragma once

#include <functional>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>

#include "tsugumi/workerd/invocation.hh"
#include "tsugumi/protocol/tsugumi_enums_generated.h"

namespace tsugumi::workerd {

// The limit counts complete JSONL entries, including their trailing newlines.
class InvocationLog {
 public:
  InvocationLog(std::ostream& output, uint64_t log_limit_bytes)
      : InvocationLog([&output](uint64_t stream, const std::string& text) {
          remote::LogJson entry;
          *entry.stream = stream;
          *entry.text = text;
          output << schema::encode(entry) << '\n';
        }, log_limit_bytes) {}

  using Sink = std::function<void(uint64_t, const std::string&)>;
  InvocationLog(Sink sink, uint64_t log_limit_bytes)
      : sink_(std::move(sink)), log_limit_bytes_(log_limit_bytes) {}

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
    sink_(stream, text);
    logged_ += encoded.size() + 1;
  }

  // A downstream runner may already have omitted bytes under this policy.
  auto truncate() -> void { truncated_ = true; }

  auto finish() -> void {
    if (!truncated_ || notice_written_) return;
    // Keep the notice outside the budget so even a one-byte limit is explicit.
    remote::LogJson entry;
    *entry.stream = static_cast<uint64_t>(Tsugumi::Data::LogStream::journal);
    *entry.text = "\n[Log output truncated at the " + std::to_string(log_limit_bytes_) + "-byte limit.]\n";
    sink_(*entry.stream, *entry.text);
    notice_written_ = true;
  }

 private:
  Sink sink_;
  uint64_t log_limit_bytes_;
  uint64_t logged_ = 0;
  bool truncated_ = false;
  bool notice_written_ = false;
};

}  // namespace tsugumi::workerd
