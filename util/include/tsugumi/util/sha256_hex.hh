#pragma once

#include <algorithm>
#include <array>
#include <compare>
#include <optional>
#include <string_view>

#include "tsugumi/util/crypto.hh"

namespace tsugumi {
// A canonical SHA-256 hex string, safe to use as a single path component.
class Sha256Hex {
 public:
  static auto parse(std::string_view text) -> std::optional<Sha256Hex> {
    if (!crypto::is_sha256_hex(text)) return std::nullopt;
    return Sha256Hex(text);
  }

  auto view() const -> std::string_view { return {text_.data(), text_.size()}; }
  auto operator<=>(const Sha256Hex&) const -> std::strong_ordering = default;

 private:
  explicit Sha256Hex(std::string_view text) {
    std::ranges::copy(text, text_.begin());
  }

  std::array<char, 64> text_;
};
}  // namespace tsugumi
