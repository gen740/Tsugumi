#include "log_window.hh"

#include <algorithm>

namespace tsugumi::linux_ui {

auto LogWindow::Tail() -> LogRequest {
  return {.read = LogRead::kTail, .tail = true, .limit_bytes = kReadBytes};
}

auto LogWindow::Earlier() const -> LogRequest {
  return {.read = LogRead::kEarlier,
          .before = start_,
          .limit_bytes = kEarlierBytes};
}

auto LogWindow::Next() const -> LogRequest {
  if (!read_ || size_ - std::min(size_, end_) > kReadBytes) return Tail();
  return {.read = LogRead::kAppend, .offset = end_, .limit_bytes = kReadBytes};
}

auto LogWindow::Apply(LogRead read, const LogPiece& piece) -> void {
  switch (read) {
    case LogRead::kTail:
      text_ = piece.text;
      start_ = piece.offset;
      end_ = piece.next_offset;
      break;
    case LogRead::kEarlier:
      if (!read_ || piece.next_offset != start_) return;
      text_.insert(0, piece.text);
      start_ = piece.offset;
      TrimBack();
      break;
    case LogRead::kAppend:
      if (!read_ || piece.offset != end_) return;
      text_ += piece.text;
      end_ = piece.next_offset;
      TrimFront();
      break;
  }
  read_ = true;
  exists_ = piece.exists;
  size_ = std::max(piece.size_bytes, end_);
}

auto LogWindow::Grew(uint64_t end_offset) -> void {
  size_ = std::max(size_, end_offset);
}

// Whole lines are dropped, so a kept line is never cut in two.
auto LogWindow::TrimFront() -> void {
  if (text_.size() <= kWindowBytes) return;
  auto cut = text_.find('\n', text_.size() - kWindowBytes);
  cut = cut == std::string::npos ? text_.size() - kWindowBytes : cut + 1;
  text_.erase(0, cut);
  start_ += cut;
}

auto LogWindow::TrimBack() -> void {
  if (text_.size() <= kWindowBytes) return;
  auto cut = text_.rfind('\n', kWindowBytes);
  cut = cut == std::string::npos ? kWindowBytes : cut + 1;
  end_ -= text_.size() - cut;
  text_.resize(cut);
}

}  // namespace tsugumi::linux_ui
