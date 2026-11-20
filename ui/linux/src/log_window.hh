// The part of a Card's log stream the log view holds: windowed reads with
// LogRangeRequest, as the Web UI and the Mac app read it. The end is read
// first (tail); earlier bytes are read on request (before = the window's
// start); a growing log is followed from the window's end when LogAvailable
// says there is more. At most kWindowBytes are kept.
#pragma once

#include <cstdint>
#include <string>

namespace tsugumi::linux_ui {

enum class LogRead { kTail, kEarlier, kAppend };

// What to put in a LogRangeRequest (besides run, Card and stream).
struct LogRequest {
  LogRead read = LogRead::kTail;
  bool tail = false;
  uint64_t offset = 0;
  uint64_t before = 0;  // 0: not given
  uint32_t limit_bytes = 0;
};

// A LogSlice's fields that matter here.
struct LogPiece {
  std::string text;
  uint64_t offset = 0;
  uint64_t next_offset = 0;
  uint64_t size_bytes = 0;
  bool exists = false;
};

class LogWindow {
 public:
  static constexpr uint32_t kReadBytes = 1U << 20;
  static constexpr uint32_t kEarlierBytes = kReadBytes >> 2;
  static constexpr uint64_t kWindowBytes = 2ULL * kReadBytes;

  [[nodiscard]] static auto Tail() -> LogRequest;
  // Earlier bytes, when the window does not start at the beginning.
  [[nodiscard]] auto Earlier() const -> LogRequest;
  // The bytes after the window, or the end again when more than one read is
  // behind.
  [[nodiscard]] auto Next() const -> LogRequest;

  // Puts a read's result in; a piece that no longer fits where the window is
  // (the stream was read again meanwhile) is dropped.
  auto Apply(LogRead read, const LogPiece& piece) -> void;
  // There are bytes after the window (a LogAvailable's end_offset).
  auto Grew(uint64_t end_offset) -> void;

  [[nodiscard]] auto text() const -> const std::string& { return text_; }
  [[nodiscard]] auto start() const -> uint64_t { return start_; }
  [[nodiscard]] auto end() const -> uint64_t { return end_; }
  [[nodiscard]] auto size() const -> uint64_t { return size_; }
  [[nodiscard]] auto exists() const -> bool { return exists_; }
  [[nodiscard]] auto read() const -> bool { return read_; }
  [[nodiscard]] auto behind() const -> bool { return size_ > end_; }

 private:
  auto TrimFront() -> void;
  auto TrimBack() -> void;

  std::string text_;
  uint64_t start_ = 0;
  uint64_t end_ = 0;
  uint64_t size_ = 0;
  bool exists_ = false;
  bool read_ = false;
};

}  // namespace tsugumi::linux_ui
